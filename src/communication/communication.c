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
#ifdef HAVE_NUMA
#include <numaif.h>
#include <numa.h>
#endif

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
//	windows_init()
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

	if(event_content != NULL && event_size>0) {
		spin_lock(&LPS[event.receiver]->in_buffer.lock);
		event.payload_offset = alloca_memoria_ingoing_buffer(event.receiver, event_size, event_content);
		if(event.payload_offset==NO_MEM)
			rootsim_error(true, "non c'è più memoria disponibile nell'extra buffer");
		//controlla se non abbiamo scritto nell'extra buffer
		if(event.payload_offset < LPS[event.receiver]->in_buffer.size)
			memcpy(LPS[event.receiver]->in_buffer.base + event.payload_offset, event_content, event_size);
		spin_unlock(&LPS[event.receiver]->in_buffer.lock);
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
		msg.size=0;

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

//@param size dimensione richiesta PER IL MESSAGGIO (e non per il blocco!)
//@param event_content da usare solo se scrivo nell'extra buffer
//@return l'offset per il PAYLOAD! (e quindi spiazzato della dimensione dell'header rispetto al blocco)
unsigned alloca_memoria_ingoing_buffer(unsigned lid, unsigned size, void* event_content){
	
	unsigned actual;
	unsigned succ;

	//devo allocare almeno una cosa di dimensione sizeof(PREV_FREE) + sizeof(succ_free)
	if(size<2*sizeof(unsigned))
		size = 2*sizeof(unsigned);
start:
	if(IS_NOT_AVAILABLE(LPS[lid]->in_buffer.first_free,lid)){
		return use_extra_buffer(size, lid, event_content);
	}
	
	actual = LPS[lid]->in_buffer.first_free;
	
	if(FREE_SIZE(actual,lid)>=size){
		//in questo caso prendo da split il valore di ritorno che il nuovo ff.
		if(actual<0 || actual>LPS[lid]->in_buffer.size || IS_NOT_AVAILABLE(actual,lid))
			printf("(%u) occhio ad actual!!\n", actual);
		LPS[lid]->in_buffer.first_free = split(actual, size, lid);
		return actual+sizeof(unsigned);
	}
	
	while(true){
		succ = NEXT_FREE_BLOCK(actual,lid);
		if(IS_NOT_AVAILABLE(succ,lid)){
			return use_extra_buffer(size, lid, event_content);
		}
		if(FREE_SIZE(succ,lid)>=size){
			if(succ<0 || succ>LPS[lid]->in_buffer.size || IS_NOT_AVAILABLE(succ,lid))
				printf("(%u) occhio a succ!!\n",lid);
			(void)split(succ, size, lid);
			return succ+sizeof(unsigned);
		}
		else
			actual = succ;
	}	
}

//@return offset "fittizio", già al netto dell'header CON HEADER E FOOTER GIA' INSERITI
//side effect: diminuisce il presence counter
//bisogna fare in modo che se il destinatario sta eseguendo concorrentemente la riallocazione questa funzione
//non deve essere chiamata... quando il destinatario rialloca devono stare tutti fermi.
unsigned use_extra_buffer(unsigned size, unsigned lid, void* event_content){
	void* ptr;
	/** questo offset al momento è "fittizio". Questo offset sarà quello giusto quando
	 ** l'extra_buffer sarà spostato nell'ingoing buffer!
	 **/
	unsigned offset = LPS[lid]->in_buffer.size;
	int i;
	atomic_add_x86(&LPS[lid]->in_buffer.extra_buffer_size_in_use, size+2*sizeof(unsigned));
	#ifdef HAVE_NUMA
	ptr = numa_alloc_onnode(size+2*sizeof(unsigned), get_numa_node(LPS[lid]->worker_thread)); 
	#else
	ptr = rsalloc(size+2*sizeof(unsigned));
	#endif
	//METTO HEADER E FOOTER
	*(unsigned*)ptr = MARK_AS_IN_USE(size);
	memcpy(ptr+sizeof(unsigned),event_content,size);
	*(unsigned*)(ptr+sizeof(unsigned)+size) = MARK_AS_IN_USE(size);
	//cerco il primo blocco libero
	for(i=0;i<EXTRA_BUFFER_SIZE;i++){
		if(CAS_x86(&LPS[lid]->in_buffer.extra_buffer[i], NULL, ptr) == true){
			return offset + sizeof(unsigned);
		}
		else{
			offset+= ( (MARK_AS_NOT_IN_USE(*(unsigned*)(LPS[lid]->in_buffer.extra_buffer[i]))) + 2 * sizeof(unsigned));
		}
	}
	return NO_MEM;
}

//@param addr è l'offset relativo all'header del blocco che sto allocando (sicuramente di dimensione sufficiente)
//@param size è la size che mi serve in quel blocco...
//@return l'addr del successivo blocco libero oppure IN_USE_FLAG se non c'è un successivo blocco libero
unsigned split(unsigned addr, unsigned size, unsigned lid){
	
//	while(atomic_read(&LPS[lid]->in_buffer.reallocation_flag)!=0);
	
	//aggiungo 2 unsigned perchè size è al netto degli header
	unsigned splitted = addr + size + 2 * sizeof(unsigned);
	unsigned addr_size = FREE_SIZE(addr,lid);
	unsigned splitted_size;
	unsigned ret = 0;
	
	if(addr_size-size!=0){ //non può essere negativo perchè so che addr può contenere size
		splitted_size = addr_size - size;		
		
		if(splitted_size<MIN_BLOCK_DIMENSION){
			size = addr_size;
			goto adegua_al_successivo;
		}
		else{
			//il posto per h e f ricordando che le size in H e F sono al netto dell'overhead
			splitted_size-= 2*sizeof(unsigned);

			*HEADER_ADDRESS_OF(splitted,lid) = splitted_size;
			*FOOTER_ADDRESS_OF(splitted, splitted_size , lid) = splitted_size;

			memcpy(NEXT_FREE_BLOCK_ADDRESS(splitted,lid), NEXT_FREE_BLOCK_ADDRESS(addr,lid), sizeof(unsigned));
			memcpy(PREV_FREE_BLOCK_ADDRESS(splitted,lid), PREV_FREE_BLOCK_ADDRESS(addr,lid), sizeof(unsigned));

			if(IS_AVAILABLE(NEXT_FREE_BLOCK(splitted,lid),lid))
				memcpy(PREV_FREE_BLOCK_ADDRESS(NEXT_FREE_BLOCK(splitted,lid),lid), &splitted, sizeof(unsigned));

			if(IS_AVAILABLE(PREV_FREE_BLOCK(splitted,lid),lid)){
				memcpy(NEXT_FREE_BLOCK_ADDRESS(PREV_FREE_BLOCK(splitted,lid),lid), &splitted, sizeof(unsigned));
				
			}
			ret = splitted;
		}
	}
	
	//se splitted è troppo piccolo o non esiste proprio
	else{
		adegua_al_successivo:
		ret = NEXT_FREE_BLOCK(addr,lid);
		if(ret==IN_USE_FLAG || (ret>=LPS[lid]->in_buffer.size) || IS_IN_USE(HEADER_OF(ret,lid)))
			ret = IN_USE_FLAG;
		//devo cambiare il prev_free a ret e dire al prev di addr che il suo succ è ora ret
		//devo inoltre dire a ret che il suo precedente è quello di addr (se addr ha un precedente libero)
		if(ret!=IN_USE_FLAG)
			memcpy(PREV_FREE_BLOCK_ADDRESS(ret,lid),PREV_FREE_BLOCK_ADDRESS(addr,lid),sizeof(unsigned));
		if(IS_AVAILABLE(PREV_FREE_BLOCK(addr,lid),lid))
			//va bene anche per ret=IN_USE.. in questo caso gli diciamo che non ha più un successivo
			memcpy(NEXT_FREE_BLOCK_ADDRESS(PREV_FREE_BLOCK(addr,lid),lid), &ret, sizeof(unsigned));
	
	}
	
	//DEVO AGGIORNARE L'HEADER E IL FOOTER DEL BLOCCO CHE HO APPENA ALLOCATO. RICORDA ANCHE L'OR CON IN USE
	*HEADER_ADDRESS_OF(addr,lid) = MARK_AS_IN_USE(size);
	*FOOTER_ADDRESS_OF(addr,size,lid) = MARK_AS_IN_USE(size);
	bzero(PAYLOAD_OF(addr,lid), size);	
	return ret;
}

//atomic needed!
//@param payload_offset offset nell'ingoing buffer del payload del blocco da liberare
void dealloca_memoria_ingoing_buffer(unsigned lid, unsigned payload_offset){
//	while(atomic_read(&LPS[lid]->in_buffer.reallocation_flag)!=0);
	if(payload_offset>=LPS[lid]->in_buffer.size)
		process_extra_buffer(lid);
	unsigned header_offset = payload_offset-sizeof(unsigned); //lavorare con questo.
	unsigned size = MARK_AS_NOT_IN_USE(HEADER_OF(header_offset,lid));	//potrebbe essere diversa da message_size se abbiamo "arrotondato" le dimensioni del blocco
	unsigned footer_offset = header_offset + size + sizeof(unsigned); //di quello che va eliminato
	unsigned prev_size;
	unsigned prev_header_offset;
	unsigned prev_footer_offset = header_offset - sizeof(unsigned); //può essere < 0
	unsigned prev_footer;
	if((int)(prev_footer_offset)>=0){
		prev_footer = *(unsigned*) (LPS[lid]->in_buffer.base + prev_footer_offset);	
	}
	else{
		prev_footer_offset = IN_USE_FLAG;
		prev_footer = IN_USE_FLAG;
	}
	unsigned succ_size;
	unsigned succ_header_offset = footer_offset + sizeof(unsigned); //può uscire dai bordi
	unsigned succ_footer_offset;
	unsigned succ_header;
	if(succ_header_offset<LPS[lid]->in_buffer.size){
		succ_header = HEADER_OF(succ_header_offset,lid);
	}
	else{
		succ_header = IN_USE_FLAG;
		succ_header_offset = IN_USE_FLAG;
	}

	//sono IN_USE_FLAG se ce li ho messi io perchè sono fuori dai bordi!!
	if(IS_IN_USE(prev_footer)){
		if(IS_IN_USE(succ_header)){
			//sia prev che succ in uso
			//caso1
			coalesce(header_offset,footer_offset,size,lid);
		}
		else{
			//prev in uso e succ no
			//caso2
			succ_size = succ_header;
			succ_footer_offset = succ_header_offset + sizeof(unsigned) + succ_size;
			delete_from_free_list(succ_header_offset,lid);
			coalesce(header_offset,succ_footer_offset,size+succ_size+2*sizeof(unsigned),lid);
		}
	}
	else if(IS_IN_USE(succ_header)){
		//succ in uso e prev no, escluso da prima
		//caso3
		prev_size = prev_footer;
		prev_header_offset = prev_footer_offset - prev_size - sizeof(unsigned);
		delete_from_free_list(prev_header_offset,lid);
		coalesce(prev_header_offset,footer_offset,size+prev_size+2*sizeof(unsigned),lid);
	}
	else{
		//nessuno in uso, caso 4
		succ_size = succ_header;
		prev_size = prev_footer;
		succ_footer_offset = succ_header_offset + succ_size + sizeof(unsigned);
		prev_header_offset = prev_footer_offset - prev_size - sizeof(unsigned);
		delete_from_free_list(prev_header_offset,lid);
		delete_from_free_list(succ_header_offset,lid);
		coalesce(prev_header_offset,succ_footer_offset,size+succ_size+prev_size+4*sizeof(unsigned),lid);
	}
}

//@param to_delete blocco da eliminare dalla free_list
void delete_from_free_list(unsigned to_delete, unsigned lid){
	if(to_delete==LPS[lid]->in_buffer.first_free)
		LPS[lid]->in_buffer.first_free = NEXT_FREE_BLOCK(LPS[lid]->in_buffer.first_free,lid);

	//io voglio dire al precedente che il next è cambiato.. devo quindi controllare che il precedente
	//sia davvero un blocco libero e non un fake pointer
	if(IS_AVAILABLE(PREV_FREE_BLOCK(to_delete,lid),lid))
		memcpy(NEXT_FREE_BLOCK_ADDRESS(PREV_FREE_BLOCK(to_delete,lid),lid), NEXT_FREE_BLOCK_ADDRESS(to_delete,lid),sizeof(unsigned));
	
	//io voglio dire al successivo che il precedente è cambiato.. devo quindi controllare che il successivo sia 
	//davvero un blocco libero e non un fake pointer
	if(IS_AVAILABLE(NEXT_FREE_BLOCK(to_delete,lid),lid))
		memcpy(PREV_FREE_BLOCK_ADDRESS(NEXT_FREE_BLOCK(to_delete,lid),lid), PREV_FREE_BLOCK_ADDRESS(to_delete,lid),sizeof(unsigned));
}

//@param header offset dell'header del nuovo blocco che si sta creando
//@param footer offset del footer del nuovo blocco che si sta creando
//@param size dimensione del nuovo blocco che si sta creando, al netto di header e footer
//alloca un nuovo segmento e lo inserisce in testa nella free list
void coalesce(unsigned header, unsigned footer, unsigned size, unsigned lid){
	bzero(PAYLOAD_OF(header,lid),size);
	*HEADER_ADDRESS_OF(header, lid) = size;
	*(unsigned*) (LPS[lid]->in_buffer.base + footer) = size;
	if(IS_AVAILABLE(LPS[lid]->in_buffer.first_free,lid))
		memcpy(PREV_FREE_BLOCK_ADDRESS(LPS[lid]->in_buffer.first_free,lid), &header, sizeof(unsigned));
	*NEXT_FREE_BLOCK_ADDRESS(header,lid) = LPS[lid]->in_buffer.first_free;
	*PREV_FREE_BLOCK_ADDRESS(header,lid) = IN_USE_FLAG;
	LPS[lid]->in_buffer.first_free = header;
}

//atomic needed
void process_extra_buffer(unsigned lid)  {
	int i;
	if(atomic_read(&LPS[lid]->in_buffer.extra_buffer_size_in_use)!=0){
		void* 	 new_ptr = NULL;
		unsigned old_size = LPS[lid]->in_buffer.size;
		unsigned actual_offset = old_size;
		unsigned new_size = 0;
		unsigned block_size = 0;
		unsigned extra_in_use;
//		atomic_set(&LPS[current_lp]->in_buffer.reallocation_flag,1);
		//ora sono in sezione critica
		extra_in_use = atomic_read(&LPS[lid]->in_buffer.extra_buffer_size_in_use);
		new_size = old_size + extra_in_use + MIN_BLOCK_DIMENSION;
		//round up new_size
		new_size |= new_size >> 1;
		new_size |= new_size >> 2;
		new_size |= new_size >> 4;
		new_size |= new_size >> 8;
		new_size |= new_size >> 16;
		++new_size;
		new_ptr = pool_realloc_memory(lid, old_size, new_size, LPS[lid]->in_buffer.base);
		LPS[lid]->in_buffer.base = new_ptr;
		LPS[lid]->in_buffer.size = new_size;
		for(i=0;i<EXTRA_BUFFER_SIZE;i++){
			if(LPS[lid]->in_buffer.extra_buffer[i]==NULL)
				break;
			block_size = MARK_AS_NOT_IN_USE(*((unsigned*) (LPS[lid]->in_buffer.extra_buffer[i]))) + 2*sizeof(unsigned);
			memcpy(new_ptr + actual_offset, LPS[lid]->in_buffer.extra_buffer[i], block_size); //copio anche header e footer
			actual_offset += block_size;
			#ifdef HAVE_NUMA
			numa_free(LPS[lid]->in_buffer.extra_buffer[i], block_size); //automaticamente fa il round up al page size
			#else
			rsfree(LPS[lid]->in_buffer.extra_buffer[i]);
			#endif
			LPS[lid]->in_buffer.extra_buffer[i]=NULL; 
		}
		//adeguo la nuova free_list con gestione LIFO (se non sono già al limite)
		if(actual_offset<LPS[lid]->in_buffer.size){
			unsigned residual_size = LPS[lid]->in_buffer.size - actual_offset - 2 * sizeof(unsigned); //al netto di h e f
			coalesce(actual_offset, actual_offset + residual_size + sizeof(unsigned), residual_size, lid);
		}
		atomic_set(&LPS[lid]->in_buffer.extra_buffer_size_in_use, 0);
	}	
}
