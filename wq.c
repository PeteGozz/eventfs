/*
   eventfs: a self-cleaning filesystem for event queues.
   Copyright (C) 2015  Jude Nelson

   This program is dual-licensed: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License version 3 or later as
   published by the Free Software Foundation. For the terms of this
   license, see LICENSE.LGPLv3+ or <http://www.gnu.org/licenses/>.

   You are free to use this program under the terms of the GNU Lesser General
   Public License, but WITHOUT ANY WARRANTY; without even the implied
   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU Lesser General Public License for more details.

   Alternatively, you are free to use this program under the terms of the
   Internet Software Consortium License, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   For the terms of this license, see LICENSE.ISC or
   <http://www.isc.org/downloads/software-support-policy/isc-license/>.
*/

#include "wq.h"

// work queue main method
static void* eventfs_wq_main( void* cls ) {
   
   struct eventfs_wq* wq = (struct eventfs_wq*)cls;
   
   struct eventfs_wreq* work_itr = NULL;
   struct eventfs_wreq* next = NULL;
   
   int rc = 0;

   while( wq->running ) {

      // is there work?
      rc = sem_trywait( &wq->work_sem );
      if( rc != 0 ) {
         
         rc = -errno;
         if( rc == -EAGAIN ) {
            
            // wait for work
            sem_wait( &wq->work_sem );
         }
         else {
            
            // some other fatal error 
            eventfs_error("FATAL: sem_trywait rc = %d\n", rc );
            break;
         }
      }
      
      // cancelled?
      if( !wq->running ) {
         break;
      }

      // exchange buffers--we have work to do
      pthread_mutex_lock( &wq->work_lock );

      work_itr = wq->work;
      wq->work = NULL;
      wq->tail = NULL;
      
      pthread_mutex_unlock( &wq->work_lock );

      // safe to use work buffer (we'll clear it out in the process)
      while( work_itr != NULL ) {

         // carry out work
         eventfs_debug("begin work %p\n", work_itr->work_data);
         rc = (*work_itr->work)( work_itr, work_itr->work_data );
         eventfs_debug("end work %p\n", work_itr->work_data);
         
         if( rc != 0 ) {
            
            eventfs_error("work %p rc = %d\n", work_itr->work, rc );
         }
         
         
         next = work_itr->next;
        
         eventfs_wreq_free( work_itr );
         eventfs_safe_free( work_itr );
         
         work_itr = next;
      }
   }

   return NULL;
}


// make a work queue  
struct eventfs_wq* eventfs_wq_new() {
   return EVENTFS_CALLOC( struct eventfs_wq, 1 );
}


// set up a work queue, but don't start it.
// return 0 on success
// return negative on failure:
// * -ENOMEM if OOM
int eventfs_wq_init( struct eventfs_wq* wq ) {

   int rc = 0;

   memset( wq, 0, sizeof(struct eventfs_wq) );
   
   rc = pthread_mutex_init( &wq->work_lock, NULL );
   if( rc != 0 ) {
      
      return -abs(rc);
   }
   
   sem_init( &wq->work_sem, 0, 0 );
   
   return rc;
}


// start a work queue
// return 0 on success
// return negative on error:
// * -EINVAL if already started
// * whatever pthread_create errors on
int eventfs_wq_start( struct eventfs_wq* wq ) {
   
   if( wq->running ) {
      return -EINVAL;
   }

   int rc = 0;
   pthread_attr_t attrs;

   memset( &attrs, 0, sizeof(pthread_attr_t) );

   wq->running = true;

   rc = pthread_create( &wq->thread, &attrs, eventfs_wq_main, wq );
   if( rc != 0 ) {

      wq->running = false;

      rc = -errno;
      eventfs_error("pthread_create errno = %d\n", rc );

      return rc;
   }

   return 0;
}

// stop a work queue
// return 0 on success
// return negative on error:
// * -EINVAL if not running
int eventfs_wq_stop( struct eventfs_wq* wq ) {

   if( !wq->running ) {
      return -EINVAL;
   }

   wq->running = false;

   // wake up the work queue so it cancels
   sem_post( &wq->work_sem );
   pthread_cancel( wq->thread );

   pthread_join( wq->thread, NULL );

   return 0;
}


// free a work request queue
static int eventfs_wq_queue_free( struct eventfs_wreq* wqueue ) {

   struct eventfs_wreq* next = NULL;
   
   while( wqueue != NULL ) {
      
      next = wqueue->next;
      
      eventfs_wreq_free( wqueue );
      eventfs_safe_free( wqueue );
      
      wqueue = next;
   }
   
   return 0;
}

// free up a work queue
// return 0 on success
// return negative on error:
// * -EINVAL if running
int eventfs_wq_free( struct eventfs_wq* wq ) {

   if( wq->running ) {
      return -EINVAL;
   }

   // free all
   eventfs_wq_queue_free( wq->work );

   pthread_mutex_destroy( &wq->work_lock );
   sem_destroy( &wq->work_sem );

   memset( wq, 0, sizeof(struct eventfs_wq) );

   return 0;
}

// create a work request
// always succeeds
int eventfs_wreq_init( struct eventfs_wreq* wreq, eventfs_wq_func_t work, void* work_data ) {

   memset( wreq, 0, sizeof(struct eventfs_wreq) );

   wreq->work = work;
   wreq->work_data = work_data;
   return 0;
}

// free a work request
int eventfs_wreq_free( struct eventfs_wreq* wreq ) {
   
   memset( wreq, 0, sizeof(struct eventfs_wreq) );
   return 0;
}

// enqueue work.  The work queue takes onwership of the wreq, so it must be malloc'ed
// always succeeds
int eventfs_wq_add( struct eventfs_wq* wq, struct eventfs_wreq* wreq ) {

   int rc = 0;

   pthread_mutex_lock( &wq->work_lock );
   
   if( wq->work == NULL ) {
      // head
      wq->work = wreq;
      wq->tail = wreq;
   }
   else {
      // append 
      wq->tail->next = wreq;
      wq->tail = wreq;
   }
   
   pthread_mutex_unlock( &wq->work_lock );

   if( rc == 0 ) {
      // have work
      sem_post( &wq->work_sem );
   }
   
   return rc;
}
