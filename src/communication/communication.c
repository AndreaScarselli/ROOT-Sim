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

	if (event_content != NULL && event_size>0) {
		spin_lock(&LPS[event.receiver]->in_buffer.lock[0]);
		do{
			event.payload_offset = alloca_memoria_ingoing_buffer(event.receiver, event_size);
		}while(event.payload_offset==NO_MEM);
		memcpy(LPS[event.receiver]->in_buffer.base[0] + event.payload_offset, event_content, event_size);
		spin_unlock(&LPS[event.receiver]->in_buffer.lock[0]);

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
/*
unsigned richiedi_altra_memoria(unsigned lid){
	unsigned ret = LPS[lid]->in_buffer.size[0];
	//quando questa viene chiamta quella maggiore è gia andata al posto 0!!
	unsigned new_size = LPS[lid]->in_buffer.size[0] * INGOING_BUFFER_GROW_FACTOR;
	pool_release_memory(lid, LPS[lid]->in_buffer.base[1]);
	LPS[lid]->in_buffer.base[1] = pool_get_memory(lid, new_size);
	LPS[lid]->in_buffer.size[1] = new_size;
	return ret;
}*/

//********************************************************************************************
//*RICORDATI CHE QUESTO DEVE RESTITUIRE L'OFFSET PER IL PAYLOAD! E NON L'OFFSET DEL MESSAGGIO*
//********************************************************************************************
//il chiamante si deve preoccupare di fare lo spinlock
unsigned alloca_memoria_ingoing_buffer(unsigned lid, unsigned size){
			
	unsigned actual;
	unsigned succ;
	unsigned new_off;
	unsigned new_size;
	int ret;
	
	//devo allocare almeno una cosa di dimensione sizeof(PREV_FREE) + sizeof(succ_free)
	if(size<2*sizeof(unsigned))
		size = 2*sizeof(unsigned);
start:
	//se FIRST_FREE è pari a IN_USE_FLAG significa che non c'è spazio libero!!
	if(IS_NOT_AVAILABLE(LPS[lid]->in_buffer.first_free,lid)){	
		ret = buffer_switch(lid);
		if(ret==NO_MEM)
			return NO_MEM;
		goto start;
	}
	
	actual = LPS[lid]->in_buffer.first_free;
	
	//ff sicuramente non è in uso, se lo fosse stato, avrei fatto la riallocazione alla precedente chiamata
	if(HEADER_OF(actual,lid)>=size){
		//in questo caso prendo da split il valore di ritorno che il nuovo ff. Split si preoccupa di riorganizzare
		//la free list
		LPS[lid]->in_buffer.first_free = split(actual, size, lid);
		//deve ritornare l'offset per il payload!
		return actual+sizeof(unsigned);
	}
	
	//a questo punto so già che actual sicuramente è un blocco libero
	while(true){
		succ = NEXT_FREE_BLOCK(actual,lid);
		//Se il successivo non è un blocco utilizzabile, non ci sarà nessun blocco utilizzabile!
		if(IS_NOT_AVAILABLE(succ,lid)){
			ret = buffer_switch(lid);
			if(ret==NO_MEM)
				return NO_MEM;
			goto start;
		}
		
		//altrimenti..(ossia se il successivo è utilizzabile)
		//se il successivo ce la fa
		if(FREE_SIZE(succ,lid)>=size){
			(void)split(succ, size, lid);
			return succ+sizeof(unsigned);
		}
		//se il successivo non ce la fa
		else
			actual = succ;
	}	
}

int buffer_switch(unsigned lid){
	if(LPS[lid]->in_buffer.size[1] < LPS[lid]->in_buffer.size[0])
		return NO_MEM;
//	printf("lid %u is going to switch buffer\n", lid);
	//cambio buffer
	spin_lock(&LPS[lid]->in_buffer.lock[1]);
	//faccio la copia
	memcpy(LPS[lid]->in_buffer.base[1], LPS[lid]->in_buffer.base[0], LPS[lid]->in_buffer.size[0]);
	unsigned size_temp = LPS[lid]->in_buffer.size[0];
	unsigned new_off = LPS[lid]->in_buffer.size[0];
	unsigned new_size = new_off - 2*sizeof(unsigned);
	LPS[lid]->in_buffer.size[0] = LPS[lid]->in_buffer.size[1];
	LPS[lid]->in_buffer.size[1] = size_temp;
	void* temp = LPS[lid]->in_buffer.base[1];
	LPS[lid]->in_buffer.base[1] = LPS[lid]->in_buffer.base[0];
	LPS[lid]->in_buffer.base[0] = temp;
	//metto header e footer al nuovo blocco 
	//che ora si trova in size[1]
	*HEADER_ADDRESS_OF(new_off,lid)=new_size;
	*FOOTER_ADDRESS_OF(new_off,new_size,lid)=new_size;
	
	//metto prev e next al nuovo blocco (che sarà ff.. politica LIFO), il new off è la vecchia size
	*PREV_FREE_BLOCK_ADDRESS(new_off,lid) = IN_USE_FLAG;
	//va bene anche nel caso in cui FF Sia IN_USE_FLAG, significherà il nostro nuovo new non ha un next
	*NEXT_FREE_BLOCK_ADDRESS(new_off,lid) = LPS[lid]->in_buffer.first_free;
	//se esisteva un first_free gli dico che adesso ha un prev ed il prev è new_off
	if(IS_AVAILABLE(LPS[lid]->in_buffer.first_free,lid))
		*PREV_FREE_BLOCK_ADDRESS(LPS[lid]->in_buffer.first_free,lid) = new_off;
	LPS[lid]->in_buffer.first_free = new_off;		
	spin_unlock(&LPS[lid]->in_buffer.lock[1]);
	return MEM_ASSIGNED;
}

//L'OPERAZIONE DI SPLIT E' UN'OPERAZIONE CHE DIVIDE UN BLOCCO DI MEMORIA E CAMBIA GLI HEADER AD ENTRAMBI
//@param addr è l'offset relativo all'header del blocco che sto allocando
//@param size è la size che mi serve in quel blocco...
//tutti i controlli che sia il blocco giusto sono fatti altrove!
//ritorna il successivo libero di addr. questo può essere quello scritto in addr (quando è ancora un blocco libero)
//o il blocco splittato (se addr è più grande di size + MIN_BLOCK_SIZE)
//questa funzione si preoccupa anche di rimettere apposto la free list

//@return l'addr del successivo blocco libero oppure IN_USE_FLAG se non c'è un successivo blocco libero
//			è interessante solo se è stato chiamato dal first_free
unsigned split(unsigned addr, unsigned size, unsigned lid){
	//aggiungo 2 unsigned perchè size è al netto degli header
	unsigned splitted = addr + size + 2 * sizeof(unsigned);
	unsigned addr_size = FREE_SIZE(addr,lid); //GIÀ AL NETTO DI HEADER E FOOTER
	unsigned splitted_size;
	int ret = 0;
	
	
	//se il blocco successivo è ancora nei limiti e non è in uso, non può essere IN_USE_FLAG
	//il flag lo usiamo solo nella free list per indicare che il prev e/o il succ non ci sono
	//se non è in uso il suo
	if(addr_size-size!=0){ //non può essere negativo perchè so che addr può contenere size
		splitted_size = addr_size - size;		
		
		if(splitted_size<MIN_BLOCK_DIMENSION){
			//lo allocherò tutto
			size = addr_size;
			//in questo caso è come se splitted non ci fosse
			goto adegua_al_successivo;
		}
		
		
		//se il blocco successivo non è fuori dai limiti (IL CONTROLLO NON LO FACCIO PERCHÈ LHO FATTO SOPRA)
		//il blocco nuovo si crea solo in questo caso
		else{
			//il posto per h e f ricordando che le size in H e F sono al netto dell'overhead
			splitted_size-= 2*sizeof(unsigned);
			//metto gli header al nuovo blocco che si è creato
			*HEADER_ADDRESS_OF(splitted,lid) = splitted_size;
			*FOOTER_ADDRESS_OF(splitted, splitted_size , lid) = splitted_size;
			//copio il next free e il prev free!!
			memcpy(NEXT_FREE_BLOCK_ADDRESS(splitted,lid), NEXT_FREE_BLOCK_ADDRESS(addr,lid), sizeof(unsigned));
			memcpy(PREV_FREE_BLOCK_ADDRESS(splitted,lid), PREV_FREE_BLOCK_ADDRESS(addr,lid), sizeof(unsigned));
			//DICO AL SUCCESSIVO CHE IL PREV SI E' spostato in avanti(se il successivo non è un uso)
			if(IS_AVAILABLE(NEXT_FREE_BLOCK(splitted,lid),lid))
				memcpy(PREV_FREE_BLOCK_ADDRESS(NEXT_FREE_BLOCK(splitted,lid),lid), &splitted, sizeof(unsigned));
			//DICO AL PREV CHE IL NEXT SI È SPOSTATO (SEMPRE SOLO SE IL PREV NON È IN USO)
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
		if(ret==IN_USE_FLAG || (ret>=LPS[lid]->in_buffer.size[0]) || IS_IN_USE(HEADER_OF(ret,lid)))
			ret = IN_USE_FLAG;
		//devo cambiare il prev_free a ret e dire al prev di addr che il suo succ è ora ret
		//devo inoltre dire a ret che il suo precedente è quello di addr (se addr ha un precedente libero)
		if(IS_AVAILABLE(PREV_FREE_BLOCK(addr,lid),lid)){
			if(ret!=IN_USE_FLAG)
				memcpy(PREV_FREE_BLOCK_ADDRESS(ret,lid),PREV_FREE_BLOCK_ADDRESS(addr,lid),sizeof(unsigned));
			//va bene anche per ret=IN_USE.. in questo caso gli diciamo che non ha più un successivo
			memcpy(NEXT_FREE_BLOCK_ADDRESS(PREV_FREE_BLOCK(addr,lid),lid), &ret, sizeof(unsigned));
		}
	
	}
	
	//DEVO AGGIORNARE L'HEADER E IL FOOTER DEL BLOCCO CHE HO APPENA ALLOCATO. RICORDA ANCHE L'OR CON IN USE
	*HEADER_ADDRESS_OF(addr,lid) = MARK_AS_IN_USE(size);
	*FOOTER_ADDRESS_OF(addr,size,lid) = MARK_AS_IN_USE(size);

	return ret;
}

//chi lo chiama si deve preoccupare di eseguire lo spinlock
//RICORDA CHE L'OFFSET E' QUELLO DEL MESSAGGIO E NON QUELLO DEL BLOCCO (CHE CORRISPONDE CON L'HEADER!!)
//STESSO DISCORSO PER SIZE! SIZE E' LA DIMENSIONE DEL MESSAGGIO! NON DEL BLOCCO!!
//NON PUOI USARLA! PUÒ DARSI CHE ABBIAMO DOVUTA INGRANDIRLA PER RIEMPIRE IL BLOCCO!!
void dealloca_memoria_ingoing_buffer(unsigned lid, unsigned payload_offset){
	unsigned header_offset = payload_offset-sizeof(unsigned); //lavorare con questo.
	unsigned size = MARK_AS_NOT_IN_USE(HEADER_OF(header_offset,lid));	//potrebbe essere diversa da message_size se abbiamo "arrotondato" le dimensioni del blocco
	unsigned footer_offset = header_offset + size + sizeof(unsigned); //di quello che va eliminato
	unsigned prev_size;
	unsigned prev_header_offset;
	unsigned prev_footer_offset = header_offset - sizeof(unsigned); //può essere < 0
	unsigned prev_footer;
	if((int)(prev_footer_offset)>=0){
		prev_footer = *(unsigned*) (LPS[lid]->in_buffer.base[0] + prev_footer_offset);	
	}
	else{
		prev_footer_offset = IN_USE_FLAG;
		prev_footer = IN_USE_FLAG;
	}
	unsigned succ_size;
	unsigned succ_header_offset = footer_offset + sizeof(unsigned); //può uscire dai bordi
	unsigned succ_footer_offset;
	unsigned succ_header;
	if(succ_header_offset<LPS[lid]->in_buffer.size[0]){
		succ_header = HEADER_OF(succ_header_offset,lid);
	}
	else{
		succ_header = IN_USE_FLAG;
		succ_header_offset = IN_USE_FLAG;
	}
	unsigned new_block_size;
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

//to delete è un blocco libero, lo elimino dalla free list perchè ci vorrò mettere un blocco che è la fusione
//di to_delete e l'adiacente che sta per essere eliminato
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

//nuovo header nuovo footer e
//size al netto di h e f
void coalesce(unsigned header, unsigned footer, unsigned size, unsigned lid){
	bzero(PAYLOAD_OF(header,lid),size);
	*HEADER_ADDRESS_OF(header, lid) = size;
	*FOOTER_ADDRESS_OF(header, size, lid) = size;
	//questa situazione può crearsi anche da delete_from_free_list
	if(IS_AVAILABLE(LPS[lid]->in_buffer.first_free,lid))
		memcpy(PREV_FREE_BLOCK_ADDRESS(LPS[lid]->in_buffer.first_free,lid), &header, sizeof(unsigned));
	*NEXT_FREE_BLOCK_ADDRESS(header,lid) = LPS[lid]->in_buffer.first_free;
	*PREV_FREE_BLOCK_ADDRESS(header,lid) = IN_USE_FLAG;
	LPS[lid]->in_buffer.first_free = header;
}
