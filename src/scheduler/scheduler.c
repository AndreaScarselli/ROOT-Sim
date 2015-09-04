/**
*			Copyright (C) 2008-2015 HPDCS Group
*			http://www.dis.uniroma1.it/~hpdcs
*
*
* This file is part of ROOT-Sim (ROme OpTimistic Simulator).
*
* ROOT-Sim is free software; you can redistribute it and/or modify it under the
* terms of the GNU General Public License as published by the Free Software
* Foundation; either version 3 of the License, or (at your option) any later
* version.
*
* ROOT-Sim is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
* A PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with
* ROOT-Sim; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
* @file scheduler.c
* @brief Re-entrant scheduler for LPs on worker threads
* @author Francesco Quaglia
* @author Alessandro Pellegrini
* @author Roberto Vitali
*/

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <datatypes/list.h>
#include <core/core.h>
#include <core/init.h>
#include <core/timer.h>
#include <arch/atomic.h>
#include <arch/ult.h>
#include <arch/thread.h>
#include <scheduler/binding.h>
#include <scheduler/process.h>
#include <scheduler/scheduler.h>
#include <scheduler/stf.h>
#include <mm/state.h>
#include <mm/dymelor.h>
#include <statistics/statistics.h>
#include <arch/thread.h>
#include <communication/communication.h>
#include <gvt/gvt.h>
#include <statistics/statistics.h>
#include <communication/communication.h> //per alloca_memoria_ingoing_buffer
#include <mm/allocator.h> //per allocate_segment
#ifdef HAVE_NUMA
#include <numaif.h>
#include <numa.h>
#endif


/// Maintain LPs' simulation and execution states
LP_state **LPS = NULL;

/// This is used to keep track of how many LPs were bound to the current KLT
__thread unsigned int n_prc_per_thread;

/// This global variable tells the simulator what is the LP currently being scheduled on the current worker thread
__thread unsigned int current_lp;

/// This global variable tells the simulator what is the LP currently being scheduled on the current worker thread
__thread simtime_t current_lvt;

/// This global variable tells the simulator what is the LP currently being scheduled on the current worker thread
__thread msg_t *current_evt;

/// This global variable tells the simulator what is the LP currently being scheduled on the current worker thread
__thread void *current_state;


/*
* This function initializes the scheduler. In particular, it relies on MPI to broadcast to every simulation kernel process
* which is the actual scheduling algorithm selected.
*
* @author Francesco Quaglia
*
* @param sched The scheduler selected initially, but master can decide to change it, so slaves must rely on what master send to them
*/
void scheduler_init(void) {

	register unsigned int i;

	// TODO: implementare con delle broadcast!!
/*	if(n_ker > 1) {
		if (master_kernel()) {
			for (i = 1; i < n_ker; i++) {
				comm_send(&rootsim_config.scheduler, sizeof(rootsim_config.scheduler), MPI_CHAR, i, MSG_INIT_MPI, MPI_COMM_WORLD);
			}
		} else {
			comm_recv(&rootsim_config.scheduler, sizeof(rootsim_config.scheduler), MPI_CHAR, 0, MSG_INIT_MPI, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		}
	}
*/

	// Allocate LPS control blocks
	LPS = (LP_state **)rsalloc(n_prc * sizeof(LP_state *));
	for (i = 0; i < n_prc; i++) {
		LPS[i] = (LP_state *)rsalloc(sizeof(LP_state));
		bzero(LPS[i], sizeof(LP_state));
		// That's the only sequentially executed place where we can set the lid
		LPS[i]->lid = i;
	}

	#ifdef HAVE_PREEMPTION
	preempt_init();
	#endif
}



static void destroy_LPs(void) {
	register unsigned int i;

	for(i = 0; i < n_prc; i++) {
//		rsfree(LPS[i]->queue_in);
//		rsfree(LPS[i]->queue_out);
//		rsfree(LPS[i]->queue_states);
//		rsfree(LPS[i]->bottom_halves);
//		rsfree(LPS[i]->rendezvous_queue);

		// Destroy stacks
		#ifdef ENABLE_ULT
		rsfree(LPS[i]->stack);
		#endif
	}

}



/**
* This function finalizes the scheduler
*
* @author Alessandro Pellegrini
*/
void scheduler_fini(void) {
	register unsigned int i;

	#ifdef HAVE_PREEMPTION
	preempt_fini();
	#endif

	destroy_LPs();

	for (i = 0; i < n_prc; i++) {
		rsfree(LPS[i]);
	}
	rsfree(LPS);

	rsfree(LPS_bound);
}



/**
* This is a LP main loop. It s the embodiment of the usrespace thread implementing the logic of the LP.
* Whenever an event is to be scheduled, the corresponding metadata are set by the <schedule>() function,
* which in turns calls <activate_LP>() to execute the actual context switch.
* This ProcessEvent wrapper explicitly returns control to simulation kernel user thread when an event
* processing is finished. In case the LP tries to access state data which is not belonging to its
* simulation state, a SIGSEGV signal is raised and the LP might be descheduled if it is not safe
* to perform the remote memory access. This is the only case where control is not returned to simulation
* thread explicitly by this wrapper.
*
* @author Francesco Quaglia
*
* @param args arguments passed to the LP main loop. Currently, this is not used.
*/
static void LP_main_loop(void *args) {
	(void)args; // this is to make the compiler stop complaining about unused args
	void* current_evt_buffer=NULL;
	
	
	// Save a default context
	#ifdef ENABLE_ULT
	context_save(&LPS[current_lp]->default_context);
	#endif
	int i = 0;

	while(true) {
		
		//extra buffer process;
		if(LPS[current_lp]->in_buffer.extra_buffer[0]!=NULL){
			puts("in process");
			void* new_ptr = NULL;
			unsigned old_size = LPS[current_lp]->in_buffer.size;
			unsigned actual_offset = old_size;
			unsigned new_size = 0;
			unsigned block_size = 0;
			unsigned extra_in_use;
			atomic_set(&LPS[current_lp]->in_buffer.reallocation_flag,1);
			while(atomic_read(&LPS[current_lp]->in_buffer.presence_counter)!=0);
			extra_in_use = atomic_read(&LPS[current_lp]->in_buffer.extra_buffer_size_in_use);
			new_size = LPS[current_lp]->in_buffer.size + extra_in_use;
			//round up new_size
			--new_size;
			new_size |= new_size >> 1;
			new_size |= new_size >> 2;
			new_size |= new_size >> 4;
			new_size |= new_size >> 8;
			new_size |= new_size >> 16;
			++new_size;
			//ora sono in sezione critica
			new_ptr = pool_realloc_memory(current_lp, old_size, new_size, LPS[current_lp]->in_buffer.base);
			LPS[current_lp]->in_buffer.base = new_ptr;
			LPS[current_lp]->in_buffer.size = new_size;
			for(i=0;i<EXTRA_BUFFER_SIZE;i++){
				if(LPS[current_lp]->in_buffer.extra_buffer[i]==NULL)
					break;
//				fprintf(stderr, "block size is %u\n", block_size);
				block_size = (*((unsigned*) (LPS[current_lp]->in_buffer.extra_buffer[i]))) + 2*sizeof(unsigned);
//				fprintf(stderr, "new_ptr is %p, actual_offset is %u, size is %u\n", new_ptr, actual_offset, LPS[current_lp]->in_buffer.size);
//				fprintf(stderr, "contiene %u\n", (*((unsigned*) (LPS[current_lp]->in_buffer.extra_buffer[i]))));
//				fprintf(stderr, "LPS[current_lp]->in_buffer.extra_buffer[i]=%p, block_size=%u\n", LPS[current_lp]->in_buffer.extra_buffer[i], block_size);
				memcpy(new_ptr + actual_offset, LPS[current_lp]->in_buffer.extra_buffer[i], block_size); //copio anche header e footer
				actual_offset += block_size;
				#ifdef HAVE_NUMA
				numa_free(LPS[current_lp]->in_buffer.extra_buffer[i], block_size); //automaticamente fa il round up al page size
				#else
				rsfree(LPS[current_lp]->in_buffer.extra_buffer[i]);
				#endif
				LPS[current_lp]->in_buffer.extra_buffer[i]=NULL; 

			}
			//adeguo la nuova free_list con gestione LIFO (se non sono già al limite)
			if(actual_offset<LPS[current_lp]->in_buffer.size){
				unsigned residual_size = LPS[current_lp]->in_buffer.size - actual_offset - 2 * sizeof(unsigned); //al netto di h e f
				//*HEADER_ADDRESS_OF(actual_offset,current_lp) = residual_size;
				//*FOOTER_ADDRESS_OF(actual_offset,current_lp) = residual_size;
				coalesce(actual_offset, actual_offset + residual_size + sizeof(unsigned), residual_size, current_lp);
			}
			atomic_set(&LPS[current_lp]->in_buffer.extra_buffer_size_in_use, 0);
			atomic_set(&LPS[current_lp]->in_buffer.reallocation_flag,0);
		}
		
		// Process the event
		timer event_timer;
		timer_start(event_timer);

		switch_to_application_mode();
		
		//devo copiarlo altrimenti magari durante l'esecuzione dell'evento viene spostato e succedono disastri
		if(current_evt->size>0){
			current_evt_buffer=rsalloc(current_evt->size);
//			fprintf(stderr, "seconda\n");
			memcpy(current_evt_buffer, LPS[current_lp]->in_buffer.base + current_evt->payload_offset, current_evt->size);
		}
		
		ProcessEvent[current_lp](LidToGid(current_lp), current_evt->timestamp, current_evt->type, 
															current_evt_buffer, current_evt->size, current_state);
		if(current_evt->size>0)
			rsfree(current_evt_buffer);
		
		
		switch_to_platform_mode();

		int delta_event_timer = timer_value_micro(event_timer);

		statistics_post_lp_data(current_lp, STAT_EVENT, 1.0);
		statistics_post_lp_data(current_lp, STAT_EVENT_TIME, delta_event_timer);

		// Give back control to the simulation kernel's user-level thread
		#ifdef ENABLE_ULT
		context_switch(&LPS[current_lp]->context, &kernel_context);
		#else
		return;
		#endif
	}
}






/**
 * This function initializes a LP execution context. It allocates page-aligned memory for efficiency
 * reasons, and then calls <context_create>() which does the final trick.
 * <context_create>() uses global variables: LPs must therefore be intialized before creating new kernel threads
 * for supporting concurrent execution of LPs.
 *
 * @author Alessandro Pellegrini
 *
 * @date November 8, 2013
 *
 * @param lp the idex of the LP in the LPs descriptor table to be initialized
 */
void initialize_LP(unsigned int lp) {
	unsigned int i;

	// Allocate LP stack
	#ifdef ENABLE_ULT
	LPS[lp]->stack = get_ult_stack(lp, LP_STACK_SIZE);
	#endif

	// Set the initial checkpointing period for this LP.
	// If the checkpointing period is fixed, this will not change during the
	// execution. Otherwise, new calls to this function will (locally) update
	// this.
	set_checkpoint_period(lp, rootsim_config.ckpt_period);


	// Initially, every LP is ready
	LPS[lp]->state = LP_STATE_READY;

	// There is no current state layout at the beginning
	LPS[lp]->current_base_pointer = NULL;

	// Initialize the queues
	LPS[lp]->queue_in = new_list(lp, msg_t);
	LPS[lp]->queue_out = new_list(lp, msg_t);
	LPS[lp]->queue_states = new_list(lp, state_t);
	LPS[lp]->bottom_halves = new_list(lp, msg_t);
	LPS[lp]->rendezvous_queue = new_list(lp, msg_t);

	// Initialize the LP lock
	spinlock_init(&LPS[lp]->lock);
	
	
	//Initialize ingoing buffer
	spinlock_init(&LPS[lp]->in_buffer.lock);
	
	
	/**
	 * 
	 *  atomic_t 	reallocation_flag;
		unsigned 	first_free;
		unsigned 	size;
		atomic_t 	extra_buffer_size_in_use; //per sapere quanto deve essere grande il nuovo buffer
		spinlock_t 	lock;
		atomic_t 	presence_counter; // presenza nell'utilizzo dell'extra buffer, in tal caso non dobbiamo ancora riallocare
		void* 		extra_buffer[EXTRA_BUFFER_SIZE];
		void*		base;
	 * 
	 * 
	 * 
	 */
	spin_lock(&LPS[lp]->in_buffer.lock);
	for(i=0;i<EXTRA_BUFFER_SIZE;i++)
		LPS[lp]->in_buffer.extra_buffer[i]=NULL;
	atomic_set(&LPS[lp]->in_buffer.reallocation_flag, 0);
	atomic_set(&LPS[lp]->in_buffer.extra_buffer_size_in_use, 0);
	atomic_set(&LPS[lp]->in_buffer.presence_counter, 0); 
	unsigned free_size = INGOING_BUFFER_INITIAL_SIZE - 2 * sizeof(unsigned);
	LPS[lp]->in_buffer.base = pool_get_memory(LPS[lp]->lid, INGOING_BUFFER_INITIAL_SIZE);
	//offset 0
	LPS[lp]->in_buffer.first_free = 0;
	//primo header, ricorda che le dimensioni sono già al netto di header e footer
	*HEADER_ADDRESS_OF(LPS[lp]->in_buffer.first_free,lp) = free_size;
	//primo footer
	*FOOTER_ADDRESS_OF(LPS[lp]->in_buffer.first_free,free_size,lp) = free_size;
	*PREV_FREE_BLOCK_ADDRESS(LPS[lp]->in_buffer.first_free,lp) = IN_USE_FLAG;
	*NEXT_FREE_BLOCK_ADDRESS(LPS[lp]->in_buffer.first_free,lp) = IN_USE_FLAG;
	LPS[lp]->in_buffer.size = INGOING_BUFFER_INITIAL_SIZE;
	spin_unlock(&LPS[lp]->in_buffer.lock);
	
	LPS[lp]->outgoing_buffer.min_in_transit = rsalloc(sizeof(simtime_t) * n_cores);
	for(i = 0; i < n_cores; i++) {
		LPS[lp]->outgoing_buffer.min_in_transit[i] = INFTY;
	}

	#ifdef HAVE_LINUX_KERNEL_MAP_MODULE
	// No read/write dependencies open so far for the LP. The current lp is always opened
	LPS[lp]->ECS_index = 0;
	LPS[lp]->ECS_synch_table[0] = lp;
	#endif

	// Create user thread
	#ifdef ENABLE_ULT
	context_create(&LPS[lp]->context, LP_main_loop, NULL, LPS[lp]->stack, LP_STACK_SIZE);
	#endif
}


void initialize_worker_thread(void) {
	register unsigned int t;

	// Divide LPs among worker threads, for the first time here
	rebind_LPs();

	if(master_thread() && master_kernel()) {
		printf("Initializing LPs... ");
		fflush(stdout);
	}

	// Initialize the LP control block for each locally hosted LP
	// and schedule the special INIT event
	for (t = 0; t < n_prc_per_thread; t++) {

		// Create user level thread for the current LP and initialize LP control block
		initialize_LP(LPS_bound[t]->lid);
		// Schedule an INIT event to the newly instantiated LP
		msg_t init_event = {
			sender: LidToGid(LPS_bound[t]->lid),
			receiver: LidToGid(LPS_bound[t]->lid),
			type: INIT,
			timestamp: 0.0,
			send_time: 0.0,
			mark: generate_mark(LidToGid(LPS_bound[t]->lid)),
			size: model_parameters.size,
			message_kind: positive,
		};

		// Copy the relevant string pointers to the INIT event payload
		if(model_parameters.size > 0) {
			spin_lock(&LPS[init_event.receiver]->in_buffer.lock);
			init_event.payload_offset = alloca_memoria_ingoing_buffer(init_event.receiver, model_parameters.size * sizeof(char *));
			memcpy(LPS[init_event.receiver]->in_buffer.base + init_event.payload_offset,  model_parameters.arguments, model_parameters.size * sizeof(char *));
			spin_unlock(&LPS[init_event.receiver]->in_buffer.lock);
		}

		(void)list_insert_head(LPS_bound[t]->lid, LPS_bound[t]->queue_in, &init_event);
		LPS_bound[t]->state_log_forced = true;
	}

	// Worker Threads synchronization barrier: they all should start working together
	thread_barrier(&all_thread_barrier);

	if(master_thread() && master_kernel())
		printf("done\n");

	register unsigned int i;
	for(i = 0; i < n_prc_per_thread; i++) {
		schedule();
	}

	// Worker Threads synchronization barrier: they all should start working together
	thread_barrier(&all_thread_barrier);

        #ifdef HAVE_PREEMPTION
        if(!rootsim_config.disable_preemption)
                enable_preemption();
        #endif

}



/**
* This function is the application-level ProcessEvent() callback entry point.
* It allows to specify which lp must be scheduled, specifying its lvt, its event
* to be executed and its simulation state.
* This provides a general entry point to application-level code, to be used
* if the LP is in forward execution, in coasting forward or in initialization.
*
* @author Alessandro Pellegrini
*
* @date November 11, 2013
*
* @param lp The id of the LP to be scheduled
* @param lvt The lvt at which the LP is scheduled
* @param evt A pointer to the event to be processed by the LP
* @param state The simulation state to be passed to the LP
*/
void activate_LP(unsigned int lp, simtime_t lvt, void *evt, void *state) {

	// Notify the LP main execution loop of the information to be used for actual simulation
	current_lp = lp;
	current_lvt = lvt;
	current_evt = evt;
	current_state = state;

//	#ifdef HAVE_PREEMPTION
//	if(!rootsim_config.disable_preemption)
//		enable_preemption();
//	#endif

	#ifdef ENABLE_ULT
	context_switch(&kernel_context, &LPS[lp]->context);
	#else
	LP_main_loop(NULL);
	#endif

//	#ifdef HAVE_PREEMPTION
//        if(!rootsim_config.disable_preemption)
//                disable_preemption();
//        #endif

	current_lp = IDLE_PROCESS;
	current_lvt = -1.0;
	current_evt = NULL;
	current_state = NULL;
}



/**
* This function checks wihch LP must be activated (if any),
* and in turn activates it. This is used only to support forward execution.
*
* @author Alessandro Pellegrini
*/
void schedule(void) {

	unsigned int lid;
	msg_t *event;
	void *state;

	#ifdef HAVE_LINUX_KERNEL_MAP_MODULE
	bool resume_execution = false;
	#endif

	// Find next LP to be executed, depending on the chosen scheduler
	switch (rootsim_config.scheduler) {

		case SMALLEST_TIMESTAMP_FIRST:
			lid = smallest_timestamp_first();
			break;

		default:
			lid = smallest_timestamp_first();
	}

//	printf("Selected LP %d with state %d\n", lid, LPS[lid]->state);

	// No logical process found with events to be processed
	if (lid == IDLE_PROCESS) {
		statistics_post_lp_data(lid, STAT_IDLE_CYCLES, 1.0);
      	return;
    }

	// If we have to rollback
    if(LPS[lid]->state == LP_STATE_ROLLBACK) {
		rollback(lid);

		LPS[lid]->state = LP_STATE_READY;
		send_outgoing_msgs(lid);
		return;
	}


	if(!is_blocked_state(LPS[lid]->state)) {
		event = advance_to_next_event(lid);
	} else {
//		printf("Riavvio %d\n", lid);
		event = LPS[lid]->bound;
	}


	// Sanity check: if we get here, it means that lid is a LP which has
	// at least one event to be executed. If advance_to_next_event() returns
	// NULL, it means that lid has no events to be executed. This is
	// a critical condition and we abort.
	if(event == NULL) {
		rootsim_error(true, "Critical condition: LP %d seems to have events to be processed, but I cannot find them. Aborting...\n", lid);
	}

	if(!process_control_msg(event)) {
		return;
	}

	state = LPS[lid]->current_base_pointer;

	#ifdef HAVE_LINUX_KERNEL_MAP_MODULE
	// In case we are resuming an interrupted execution, we keep track of this.
	// If at the end of the scheduling the LP is not blocked, we can unblokc all the remote objects
	if(LPS[lid]->state == LP_STATE_READY_FOR_SYNCH) {
		resume_execution = true;
	}
	#endif

	// Schedule the LP user-level thread
	LPS[lid]->state = LP_STATE_RUNNING;
	activate_LP(lid, lvt(lid), event, state);
	if(!is_blocked_state(LPS[lid]->state)) {
		LPS[lid]->state = LP_STATE_READY;
		send_outgoing_msgs(lid);
	}

	#ifdef HAVE_LINUX_KERNEL_MAP_MODULE
	if(resume_execution && !is_blocked_state(LPS[lid]->state)) {
		unblock_synchronized_objects(lid);
		// This is to avoid domino effect when relying on rendezvous messages
		force_LP_checkpoint(lid);
	}
	#endif

	// Log the state, if needed
	LogState(lid);

}
