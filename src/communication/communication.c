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
* @file communication.c
* @brief This module implements all the communication routines, for exchanging
*        messages among different logical processes and simulator instances.
* @author Francesco Quaglia
* @author Roberto Vitali
*
*/

#include <stdlib.h>
#include <float.h>

#include <core/core.h>
#include <gvt/gvt.h>
#include <queues/queues.h>
#include <communication/communication.h>
#include <statistics/statistics.h>
#include <scheduler/scheduler.h>
#include <scheduler/process.h>
#include <datatypes/list.h>
#include <mm/dymelor.h>
#include <mm/allocator.h> //per allocate_segment

/// This is the function pointer to correctly set ScheduleNewEvent API version, depending if we're running serially or parallelly
void (* ScheduleNewEvent)(unsigned int gid_receiver, simtime_t timestamp, unsigned int event_type, void *event_content, unsigned int event_size);

/// Buffer used by MPI for outgoing messages
//static char buff[SLOTS * sizeof(msg_t)];


/**
* This function initializes the communication subsystem
*
* @author Roberto Vitali
*/
void communication_init(void) {
//	windows_init();
	int i;
	for(i=0;i<n_prc;i++){
		unsigned free_size = INGOING_BUFFER_INITIAL_SIZE - 2 * sizeof(unsigned);
		LPS[i]->in_buffer.base = pool_get_memory(LPS[i]->lid, INGOING_BUFFER_INITIAL_SIZE);
		//offset 0
		LPS[i]->in_buffer.first_free = 0;
		
		//primo header, ricorda che le dimensioni sono già al netto di header e footer
		memcpy(LPS[i]->in_buffer.base, &free_size, sizeof(unsigned));
		//primo footer
		memcpy(LPS[i]->in_buffer.base + INGOING_BUFFER_INITIAL_SIZE - sizeof(unsigned), &free_size, sizeof(unsigned));
		LPS[i]->in_buffer.size = INGOING_BUFFER_INITIAL_SIZE;
	}
}


/**
* This function finalizes the communication subsystem
*
* @author Roberto Vitali
*
*/
void communication_fini(void) {
}





/**
* This function is invoked by the application level software to inject new events into the simulation
*
* @author Francesco Quaglia
*
* @param gid_receiver Global id of logical process at which the message must be delivered
* @param timestamp Logical Virtual Time associated with the event enveloped into the message
* @param event_type Type of the event
* @param event_content Payload of the event
* @param event_size Size of event's payload
*/
void ParallelScheduleNewEvent(unsigned int gid_receiver, simtime_t timestamp, unsigned int event_type, void *event_content, unsigned int event_size) {
	msg_t event;

	switch_to_platform_mode();

	// In Silent execution, we do not send again already sent messages
	if(LPS[current_lp]->state == LP_STATE_SILENT_EXEC) {
		return;
	}

	// Check whether the destination LP is out of range
	if(gid_receiver > n_prc_tot - 1) {	// It's unsigned, so no need to check whether it's < 0
		rootsim_error(false, "Warning: the destination LP %d is out of range. The event has been ignored\n", gid_receiver);
		goto out;
	}

	// Check if the associated timestamp is negative
	if(timestamp < lvt(current_lp)) {
		rootsim_error(true, "LP %d is trying to generate an event (type %d) to %d in the past! (Current LVT = %f, generated event's timestamp = %f) Aborting...\n", current_lp, event_type, gid_receiver, lvt(current_lp), timestamp);
	}

	// Copy all the information into the event structure
	bzero(&event, sizeof(msg_t));
	event.sender = LidToGid(current_lp);
	event.receiver = gid_receiver;
	event.type = event_type;
	event.timestamp = timestamp;
	event.send_time = lvt(current_lp);
	event.message_kind = positive;
	event.mark = generate_mark(current_lp);
	event.size = event_size;

	if(event.type == RENDEZVOUS_START) {
		event.rendezvous_mark = current_evt->rendezvous_mark;
	}

	if (event_content != NULL && event_size>0) {
		spin_lock(&(LPS[event.receiver]->in_buffer.lock));
		event.payload_offset = alloca_memoria_ingoing_buffer(event.receiver, event_size);
		memcpy((LPS[event.receiver]->in_buffer.base) + event.payload_offset, event_content, event_size);
		spin_unlock(&(LPS[event.receiver]->in_buffer.lock));

	}

	insert_outgoing_msg(&event);

    out:
	switch_to_application_mode();
}



/**
* This function send all the antimessages for a certain LP.
* After the antimessage is sent, the header is removed from the output queue!
*
* @author Francesco Quaglia
*
* @param lid The Logical Process Id
*/
void send_antimessages(unsigned int lid, simtime_t after_simtime) {
	msg_t *anti_msg,
		  *anti_msg_next;

	msg_t msg;

	if (list_empty(LPS[lid]->queue_out))
		return;

	// Get the first message header with a timestamp <= after_simtime
	anti_msg = list_tail(LPS[lid]->queue_out);
	while(anti_msg != NULL && anti_msg->send_time > after_simtime)
		anti_msg = list_prev(anti_msg);

	// The next event is the first event with a sendtime > after_simtime, if any
	if(anti_msg == NULL)
		return;

	anti_msg = list_next(anti_msg);

	// Now send all antimessages
	while(anti_msg != NULL) {
		bzero(&msg, sizeof(msg_t));
		msg.sender = anti_msg->sender;
		msg.receiver = anti_msg->receiver;
		msg.timestamp = anti_msg->timestamp;
		msg.send_time = anti_msg->send_time;
		msg.mark = anti_msg->mark;
		msg.message_kind = negative;

		Send(&msg);

		// Remove the already sent antimessage from output queue
		anti_msg_next = list_next(anti_msg);
		list_delete_by_content(lid, LPS[lid]->queue_out, anti_msg);
		anti_msg = anti_msg_next;
	}
}





/**
*
*
* @author Roberto Vitali
*/
int comm_finalize(void) {

	register unsigned int i;

	// Release as well memory used for remaining input/output queues
	for(i = 0; i < n_prc; i++) {
		while(!list_empty(LPS[i]->queue_in)) {
			list_pop(i, LPS[i]->queue_in);
		}
		while(!list_empty(LPS[i]->queue_out)) {
			list_pop(i, LPS[i]->queue_out);
		}
	}

//	return MPI_Finalize();
	return 0;

}



/**
* Send a message. If it's scheduled to a local LP, update its queue, otherwise
* ask MPI to deliver it to the hosting kernel instance.
*
* @author Francesco Quaglia
*/
void Send(msg_t *msg) {
	// Check whether the message recepient is local or remote
	if(GidToKernel(msg->receiver) == kid) { // is local
		insert_bottom_half(msg);
	} else { // is remote
		rootsim_error(true, "Calling an operation not yet reimplemented, this should never happen!\n", __FILE__, __LINE__);
	}
}



/**
* This function allows kernels to receive time barrier from other instances and calculate
* the maximum value.
*
* @author Francesco Quaglia
*/
/*simtime_t receive_time_barrier(simtime_t max) {
	register unsigned int i;
	simtime_t tmp;

	// Compute the maximum time barrier
	for (i = 0; i < n_ker; i++) {
		if  (i != kid) {
			(void)comm_recv((char*)&tmp, sizeof(simtime_t), MPI_CHAR, (int)i, MSG_TIME_BARRIER, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			if (D_DIFFER(max, -1) || ((tmp - 1) > DBL_EPSILON && tmp > max))
				max = tmp;
		}
	}

	return max;
}*/




#if 0
/**
*
*
* @author Francesco Quaglia
*/
static bool pending_msg(void) {

    int flag = 0;

    (void)comm_probe(MPI_ANY_SOURCE, MSG_EVENT, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    return (bool)flag;

}


/**
*
*
* @author Francesco Quaglia
*/
int messages_checking(void) {
	msg_t msg;
	int at_least_one_message = 0;


	// Are we in GVT computation phase?
	// TODO a cosa serve questa parte qui sotto?!
//	if (ComputingGvt() == false)
//		send_forced_ack();


	// Check whether we have received any ack message
	receive_ack();

	// If we have pending messages, we process all of them in order to update the queues
 	while(pending_msg()){

		at_least_one_message = 1;

		// Receive the message
	    	(void)comm_recv((char*)&msg, sizeof(msg_t), MPI_CHAR, MPI_ANY_SOURCE, MSG_EVENT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		// For GVT Operations
		gvt_messages_rcvd[GidToKernel(msg.sender)] += 1;


		// Update the queues
//		DataUpdating(&msg);

	}

	// Did we receive at least one message?
	return at_least_one_message;

}
#endif





/**
*
*
* @author Francesco Quaglia
*/
void insert_outgoing_msg(msg_t *msg) {

	// Sanity check
	if(LPS[current_lp]->outgoing_buffer.size == MAX_OUTGOING_MSG){
		rootsim_error(true, "Outgoing message queue full! Too many events scheduled by one single event. Aborting...");
	}

	// Message structure was declared on stack in ScheduleNewEvent: make a copy!
	LPS[current_lp]->outgoing_buffer.outgoing_msgs[LPS[current_lp]->outgoing_buffer.size++] = *msg;

	// Store the minimum timestamp of outgoing messages
	if(msg->timestamp < LPS[current_lp]->outgoing_buffer.min_in_transit[LPS[current_lp]->worker_thread]) {
		LPS[current_lp]->outgoing_buffer.min_in_transit[LPS[current_lp]->worker_thread] = msg->timestamp;
	}
}



void send_outgoing_msgs(unsigned int lid) {

	register unsigned int i = 0;
	msg_t *msg;

	for(i = 0; i < LPS[lid]->outgoing_buffer.size; i++) {
		msg = &LPS[lid]->outgoing_buffer.outgoing_msgs[i];
		Send(msg);

		// Register the message in the sender's output queue, for antimessage management
		(void)list_insert(msg->sender, LPS[msg->sender]->queue_out, send_time, msg);
	}

	LPS[lid]->outgoing_buffer.size = 0;
}

//@return il primo offset del nuovo blocco (che corrisponde alla size precedente). 
unsigned richiedi_altra_memoria(unsigned lid){
//	spin_lock(&LPS[lid]->in_buffer.lock);
	unsigned ret = LPS[lid]->in_buffer.size;
	unsigned new_size = (LPS[lid]->in_buffer.size) * INGOING_BUFFER_GROW_FACTOR;
	LPS[lid]->in_buffer.base = pool_realloc_memory(lid, new_size, LPS[lid]->in_buffer.base);
	LPS[lid]->in_buffer.size = new_size;
	return ret;
//	spin_unlock(&LPS[lid]->in_buffer.lock);
}

unsigned alloca_memoria_ingoing_buffer(unsigned lid, unsigned size){
	//il chiamante si deve preoccupare di fare lo spinlock
	unsigned ptr_offset;
	ptr_offset = assegna_blocco(lid,size);
	return ptr_offset;
}

//L'OPERAZIONE DI SPLIT E' UN'OPERAZIONE CHE DIVIDE UN BLOCCO DI MEMORIA E CAMBIA GLI HEADER AD ENTRAMBI
//param addr è l'offset relativo all'header del blocco che sto allocando
//param size è la size che mi serve in quel blocco...
//tutti i controlli che sia il blocco giusto sono fatti altrove!
//ritorna il successivo libero di addr. questo può essere quello scritto in addr (quando è ancora un blocco libero)
//o il blocco splittato (se addr è più grande di size + MIN_BLOCK_SIZE)

//@return l'addr del successivo blocco libero oppure IN_USE_FLAG se non c'è un successivo blocco libero
unsigned split(unsigned addr, unsigned* size, unsigned lid){
	
	//aggiungo 2 unsigned perchè size è al netto degli header
	unsigned splitted = addr + 2 * sizeof(unsigned) + (*size);
	unsigned addr_size = FREE_SIZE(addr,lid); //GIÀ AL NETTO DI HEADER E FOOTER
	
	unsigned splitted_size;
	int ret = 0;
	unsigned size_with_flag;
	
	//se il blocco successivo è ancora nei limiti e non è in uso
	if((splitted < LPS[lid]->in_buffer.size) && ((HEADER_OF(splitted,lid) & IN_USE_FLAG) == 0)){
		splitted_size = addr_size - *size;		
		 
		if(splitted_size<MIN_BLOCK_DIMENSION){
			//lo allocherò tutto
			*size = addr_size;
		}
		
		
		//se il blocco successivo non è fuori dai limiti (IL CONTROLLO NON LO FACCIO XKE LHO FATTO SOPRA)
		//ed è sufficientemente grande (splitted_size>=0)
		//il blocco nuovo si crea solo in questo caso
		else{
			//il posto per h e f ricordando che le size in H e F sono al netto dell'overhead
			splitted_size-= 2*sizeof(unsigned);
			//metto gli header al nuovo blocco che si è creato
			memcpy(LPS[lid]->in_buffer.base + splitted, &splitted_size, sizeof(unsigned));
			memcpy(LPS[lid]->in_buffer.base + splitted + splitted_size + sizeof(unsigned), &splitted_size, sizeof(unsigned));
			ret = splitted;
		}
	}
	
	//se splitted è troppo piccolo o non esiste proprio
	else
		ret = NEXT_FREE_BLOCK(addr,lid);
	
	if(IS_IN_USE(HEADER_OF(ret,lid)) || (ret>=LPS[lid]->in_buffer.size)){
		ret = IN_USE_FLAG;
	}
	else{
		//vado dal successivo a dirgli che il suo prev_free è ora il mio prev_free
		//ovviamente devo controllare che il successivo sia davvero un blocco libero
		unsigned succ_free = NEXT_FREE_BLOCK(ret,lid);
		unsigned prev_free_di_ret = PREV_FREE_BLOCK(ret,lid);
		if((!IS_IN_USE(HEADER_OF(succ_free, lid))) && (succ_free!=IN_USE_FLAG) && HEADER_OF(succ_free,lid)!=0){
//			puts("qua ci vado");
			memcpy(LPS[lid]->in_buffer.base + succ_free + sizeof(unsigned), &prev_free_di_ret, sizeof(unsigned));
		}
	}
	
	//DEVO AGGIORNARE L'HEADER E IL FOOTER DEL BLOCCO CHE HO APPENA ALLOCATO. RICORDA ANCHE L'OR CON IN USE
	size_with_flag = MARK_AS_IN_USE(*size);
	memcpy(LPS[lid]->in_buffer.base + addr, &size_with_flag, sizeof(unsigned));
	memcpy(LPS[lid]->in_buffer.base + addr + sizeof(unsigned) + (*size), &size_with_flag, sizeof(unsigned));

	return ret;
}

//*****************************************************************************************
//RICORDATI CHE QUESTO DEVE RESTITUIRE L'OFFSET PER IL PAYLOAD! E NON L'OFFSET DEL MESSAGGIO
//*****************************************************************************************
unsigned assegna_blocco(unsigned lid, unsigned size){
	
	unsigned actual;
	unsigned succ;
	unsigned new_off;
	unsigned new_size;
	
	//devo allocare almeno una cosa di dimensione sizeof(PREV_FREE) + sizeof(succ_free)
	if(size<2*sizeof(unsigned))
		size = 2*sizeof(unsigned);
	
	//se FIRST_FREE è pari a IN_USE_FLAG significa che non c'è spazio libero!!
	if(LPS[lid]->in_buffer.first_free==IN_USE_FLAG){
		LPS[lid]->in_buffer.first_free = richiedi_altra_memoria(lid);
	}
	
	actual = LPS[lid]->in_buffer.first_free;
	//caso in cui FF ce la fa
	if(HEADER_OF(actual,lid)>=size){
		LPS[lid]->in_buffer.first_free = split(actual, &size, lid);
		//deve ritornare l'offset per il payload!
		return actual+sizeof(unsigned);
	}
	
	//altrimenti, se FF non ce la fa
	
	int i=0;
	
	while(true){
		succ = NEXT_FREE_BLOCK(actual,lid);
		//Se il successivo non è un blocco utilizzabile
		if(succ==IN_USE_FLAG || IS_IN_USE(HEADER_OF(succ,lid))){
			
			//QUESTA SCRITTA DI DEBUG PUO' ora provocare segfault
			new_off = richiedi_altra_memoria(lid);
			new_size = new_off - 2*( (unsigned)sizeof(unsigned)); //al netto di h e f
			//devo dare al nuovo blocco l'header e il footer
			memcpy(LPS[lid]->in_buffer.base + new_off, &new_size, sizeof(unsigned));
			memcpy(LPS[lid]->in_buffer.base + 2*new_off - sizeof(unsigned), &new_size, sizeof(unsigned));

			//devo dire ad actual chi è il nuovo libero succesivo
			memcpy(LPS[lid]->in_buffer.base + actual + 2*sizeof(unsigned), &new_off, sizeof(unsigned));
			continue;
		}
		
		//altrimenti..(ossia se il successivo è utilizzabile)
		//se il successivo ce la fa
		if(FREE_SIZE(succ,lid)>=size){
			unsigned succ_succ = split(succ, &size, lid);
			//cambio il successivo ad actual
			memcpy(LPS[lid]->in_buffer.base + 2*sizeof(unsigned) + actual, &succ_succ, sizeof(unsigned));
			//deve ritornare l'offset per il payload!
			return succ+sizeof(unsigned);
		}
		//se il successivo non ce la fa
		else{
			actual = succ;
		}
	}	
	
}

//chi lo chiama si deve preoccupare di eseguire lo spinlock
//occhio che questa non deve fare munmap eh
//RICORDA CHE L'OFFSET E' QUELLO DEL MESSAGGIO E NON QUELLO DEL BLOCCO (CHE CORRISPONDE CON L'HEADER!!)
void dealloca_memoria_ingoing_buffer(unsigned lid, unsigned payload_offset, int size){
	
}
