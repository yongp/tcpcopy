#include "../core/xcopy.h"
#include "../communication/msg.h"
#include "session.h"

static char      *pool, item[MAX_MTU + MAX_MTU];
static int       raw_sock, read_over_flag = 1;
size_t           pool_max_addr, pool_size, pool_fact;
static uint64_t  read_cnt = 0, write_cnt = 0, event_cnt = 0;
static uint64_t  packs_put_cnt = 0, raw_packs = 0, valid_raw_packs = 0;
static uint64_t  recv_pack_cnt_from_pool = 0;

#if (MULTI_THREADS)  
static pthread_mutex_t mutex;
static pthread_cond_t  empty, full;
static pthread_t       work_tid; 
#endif

/*
 * Put the packet to the buffered pool
 */
static void put_packet_to_pool(const char *packet, int len)
{
	int       act_len = len, next_w_pointer = 0, w_pointer = 0;
	int       *size_p = NULL;
	uint64_t  next_w_cnt = 0, diff = 0;
	char      *p = NULL;


	packs_put_cnt++;

#if (MULTI_THREADS)  
	pthread_mutex_lock(&mutex);
#endif
	next_w_cnt     = write_cnt + len + sizeof(int);	
	next_w_pointer = next_w_cnt%pool_size;
	if(next_w_pointer > pool_max_addr){
		next_w_cnt = (next_w_cnt/pool_size + 1) << pool_fact;
		len += pool_size - next_w_pointer;
	}
	if(len > 65535){
		log_info(LOG_NOTICE, "len is %d", len);
	}
	diff = next_w_cnt - read_cnt;
	while(1){
		if(diff > pool_size){
			log_info(LOG_ERR, "pool is full");
			log_info(LOG_ERR, "read:%llu, write:%llu, next_w_cnt:%llu",
					read_cnt, write_cnt, next_w_cnt);

#if (MULTI_THREADS)  
			pthread_cond_wait(&empty, &mutex);
#endif
		}else
		{
			break;
		}
		diff = next_w_cnt - read_cnt;
	}
	w_pointer = write_cnt % pool_size;
	size_p    = (int*)(pool + w_pointer);
	p         = pool + w_pointer + sizeof(int);
	write_cnt = next_w_cnt;
	/* Put packet to pool */
	memcpy(p, packet, act_len);
	*size_p   = len;
#if (MULTI_THREADS)  
	pthread_cond_signal(&full);
	pthread_mutex_unlock(&mutex);
#endif
}

/*
 * Get one packet from buffered pool
 */
static char *get_pack_from_pool()
{
	int  read_pos, len;
	char *p;

	recv_pack_cnt_from_pool++;
	read_over_flag = 0;

#if (MULTI_THREADS)  
	pthread_mutex_lock (&mutex);
#endif
	if(read_cnt >= write_cnt){
		read_over_flag = 1;

#if (MULTI_THREADS)  
		pthread_cond_wait(&full, &mutex);
#endif
	}
	read_pos = read_cnt%pool_size;
	p        = pool + read_pos + sizeof(int);
	len      = *(int*)(pool + read_pos);
	if(len >65535){
		log_info(LOG_ERR, "len is too long:%d", len);
	}
	memcpy(item, p, len);
	read_cnt = read_cnt + len + sizeof(int);

#if (MULTI_THREADS)  
	pthread_cond_signal(&empty);
	pthread_mutex_unlock (&mutex);
#endif

	/* The packet minimum length is 40 bytes */
	if(len < 40){
		log_info(LOG_WARN, "packet len is less than 40");
	}

	return item;
}

/*
 * Process packets here
 */
static void *dispose(void *thread_id)
{
	char *packet;

	/* Init session table*/
	session_table_init();
	
	/* Give a hint to terminal */
	printf("I am booted\n");
	if(NULL != thread_id){
		log_info(LOG_NOTICE, "booted,tid:%d", *((int*)thread_id));
	}else{
		log_info(LOG_NOTICE, "I am booted with no thread id");
	}
	/* Loop */
	while(1){
		packet = get_pack_from_pool();
		process(packet);
	}

	return NULL;
}

static void set_nonblock(int socket)
{
	int flags;
	flags = fcntl(socket, F_GETFL, 0);
	fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

/* Initiate input raw socket */
static int init_raw_socket()
{
	int       sock, recv_buf_opt, ret;
	socklen_t opt_len;
#if (COPY_LINK_PACKETS)
	/* 
	 * AF_PACKET
	 * Packet sockets are used to receive or send raw packets 
	 * at the device driver level.They allow the user to 
	 * implement protocol modules in user space on top of 
	 * the physical layer. 
	 * ETH_P_IP
	 * Internet Protocol packet that is related to the Ethernet 
	 */
	sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
#else 
	/* copy ip datagram from IP layer*/
	sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
#endif
	if(-1 == sock){
		perror("socket");
		log_info(LOG_ERR, "%s", strerror(errno));	
	}
	set_nonblock(sock);
	recv_buf_opt   = 67108864;
	opt_len = sizeof(int);
	ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buf_opt, opt_len);
	if(-1 == ret){
		perror("setsockopt");
		log_info(LOG_ERR, "setsockopt:%s", strerror(errno));	
	}

	return sock;
}

/* Replicate packets for multiple-copying */
static int replicate_packs(const char *packet,int length, int replica_num)
{
	int           i;
	struct tcphdr *tcp_header;
	struct iphdr  *ip_header;
	uint32_t      size_ip;
	uint16_t      orig_port, addition, dest_port, rand_port;
	
	ip_header  = (struct iphdr*)packet;
	size_ip    = ip_header->ihl << 2;
	tcp_header = (struct tcphdr*)((char *)ip_header + size_ip);
	orig_port  = ntohs(tcp_header->source);

#if (DEBUG_TCPCOPY)
	log_info(LOG_DEBUG, "orig port:%u", orig_port);
#endif
	rand_port = clt_settings.rand_port_shifted;
	for(i = 1; i < replica_num; i++){
		addition   = (1024 << ((i << 1)-1)) + rand_port;
		dest_port  = get_appropriate_port(orig_port, addition);
#if (DEBUG_TCPCOPY)
		log_info(LOG_DEBUG, "new port:%u", dest_port);
#endif
		tcp_header->source = htons(dest_port);
		put_packet_to_pool((const char*)packet, length);
	}

	return 0;

}

/*
 * Retrieve raw packets
 */
static int retrieve_raw_sockets(int sock)
{

	char     recv_buf[RECV_BUF_SIZE], tmp_packet[MAX_MTU];
	char     *packet;
	int      replica_num, i, last, err, recv_len, packet_num, max_payload;
	uint16_t size_ip, size_tcp, tot_len, cont_len, pack_len;
	uint32_t seq;
	struct tcphdr *tcp_header;
	struct iphdr  *ip_header;

	while(1){
		recv_len = recvfrom(sock, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
		if(recv_len < 0){
			err = errno;
			if(EAGAIN == err){
				break;
			}
			perror("recvfrom");
			log_info(LOG_ERR, "recvfrom:%s", strerror(errno));
		}
		if(0 == recv_len){
			log_info(LOG_ERR, "recv len is 0");
			break;
		}
		raw_packs++;
		if(recv_len > RECV_BUF_SIZE){
			log_info(LOG_ERR, "recv_len:%d ,it is too long", recv_len);
			break;
		}
		packet = recv_buf;
		if(is_packet_needed((const char *)packet)){
			valid_raw_packs++;
			replica_num = clt_settings.replica_num;
#if (MULTI_THREADS)  
			packet_num = 1;
			/* 
			 * If packet length larger than 1500, then we split it. 
			 * This is to solve the ip fragmentation problem
			 */
			if(recv_len > clt_settings.mtu){
				/* Calculate number of packets */
				ip_header   = (struct iphdr*)packet;
				size_ip     = ip_header->ihl << 2;
				tot_len     = ntohs(ip_header -> tot_len);
				if(tot_len != recv_len){
					log_info(LOG_WARN, "packet len:%u, recv len:%u",
							tot_len, recv_len);
					break;
				}
				tcp_header  = (struct tcphdr*)((char *)ip_header + size_ip);
				size_tcp    = tcp_header->doff << 2;
				cont_len    = tot_len - size_tcp - size_ip;
				max_payload = clt_settings.mtu - size_tcp - size_ip;
				packet_num  = (cont_len + max_payload - 1)/max_payload;
				seq         = ntohl(tcp_header->seq);
				last        = packet_num - 1;
				for(i = 0 ; i < packet_num; i++){
					tcp_header->seq = htonl(seq + i * max_payload);
					if(i != last){
						pack_len = clt_settings.mtu;
					}else{
						pack_len += (cont_len - packet_num * max_payload);
					}
					ip_header->tot_len = htons(pack_len);
					put_packet_to_pool((const char*)packet, pack_len);
					if(replica_num > 1){
						memcpy(tmp_packet, packet, pack_len);
						replicate_packs(tmp_packet, pack_len, replica_num);
					}
				}
			}else{
				put_packet_to_pool((const char*)packet, recv_len);
				/* Multi-copying is only supported in multithreading mode */
				if(replica_num > 1){
					replicate_packs(packet, recv_len, replica_num);
				}
			}
#else
			process(packet);
#endif
		}

		if(raw_packs%100000 == 0){
			log_info(LOG_NOTICE,
					"raw packets:%llu, valid :%llu, total in pool:%llu",
					raw_packs, valid_raw_packs, packs_put_cnt);
		}
	}

	return 0;
}

/* Check resource usage, such as memory usage and cpu usage */
static void check_resource_usage()
{
	int           who = RUSAGE_SELF;
	struct rusage usage;
	int           ret;
	ret = getrusage(who, &usage);
	if(-1 == ret){
		perror("getrusage");
		log_info(LOG_ERR, "getrusage:%s", strerror(errno));	
	}
	/* Total amount of user time used */
	log_info(LOG_NOTICE, "user time used:%ld",usage.ru_utime.tv_sec);
	/* Total amount of system time used */
	log_info(LOG_NOTICE, "sys  time used:%ld",usage.ru_stime.tv_sec);
	/* Maximum resident set size (in kilobytes) */
	log_info(LOG_NOTICE, "max memory size:%ld",usage.ru_maxrss);
	if(usage.ru_maxrss > clt_settings.max_rss){
		log_info(LOG_WARN, "occupies too much memory,limit:%ld",
				clt_settings.max_rss);
	}
}

/* Dispose one event*/
static void dispose_event(int fd)
{
	struct msg_server_s *msg;
	int                 pid;
	char                path[512];

	event_cnt++;
	if(fd == raw_sock){
		retrieve_raw_sockets(fd);
	}else{
		msg = msg_client_recv(fd);
		if(NULL == msg ){
			fprintf(stderr, "NULL msg :\n");
			log_info(LOG_ERR, "NULL msg from msg_client_recv");
			exit(1);
		}   
#if (MULTI_THREADS)  
		put_packet_to_pool((const char*)msg, sizeof(struct msg_server_s));
#else
		process((char*)msg);
#endif
	}   
	if((event_cnt%1000000) == 0){
		check_resource_usage();
	}
}

void tcp_copy_exit()
{
	int i;
#if (MULTI_THREADS)  
	if(0 != pthread_join(work_tid, NULL)){
		perror("join error");
	}
#endif
	session_table_destroy();
	if(-1 != raw_sock){
		close(raw_sock);
		raw_sock = -1;
	}
	send_close();
	log_end();
	if(NULL != pool){
		free(pool);
		pool = NULL;
	}
	if(clt_settings.raw_transfer != NULL){
		free(clt_settings.raw_transfer);
		clt_settings.raw_transfer = NULL;
	}
	if(clt_settings.log_path != NULL){
		free(clt_settings.log_path);
		clt_settings.log_path = NULL;
	}
#ifdef TCPCOPY_MYSQL_ADVANCED
	if(clt_settings.user_pwd != NULL){
		free(clt_settings.user_pwd);
		clt_settings.user_pwd = NULL;
	}
#endif
	if(clt_settings.transfer.mappings != NULL){
		for(i = 0; i < clt_settings.transfer.num; i++){
			free(clt_settings.transfer.mappings[i]);
		}
		free(clt_settings.transfer.mappings);
		clt_settings.transfer.mappings = NULL;
	}
	exit(0);

}

void tcp_copy_over(const int sig)
{
	int total = 0;

	log_info(LOG_WARN, "sig %d received", sig);
	while(!read_over_flag){
		sleep(1);
		total++;
		/* Wait for 30 seconds */
		if(total > 30){
			break;
		}
	}
	exit(0);
}


/* Initiate tcpcopy client */
int tcp_copy_init()
{
	int                    i, ret;
	ip_port_pair_mapping_t *pair;
	ip_port_pair_mapping_t **mappings;
	uint16_t               online_port, target_port;
	uint32_t               target_ip;

#if (MULTI_THREADS)  
	pthread_attr_t         attr;
#endif

	select_sever_set_callback(dispose_event);

	/* Init pool */
	pool_fact = clt_settings.pool_fact;
	pool_size = 1 << pool_fact;
	pool_max_addr = pool_size - MAX_MTU;
	pool = (char*)calloc(1, pool_size);

	/* Init input raw socket info */
	raw_sock = init_raw_socket();
	if(raw_sock != -1){
		/* Add the input raw socket to select */
		select_sever_add(raw_sock);
		/* Init output raw socket info */
		send_init();
#if (MULTI_THREADS)  
		pthread_mutex_init(&mutex, NULL);
		pthread_cond_init(&full, NULL);
		pthread_cond_init(&empty, NULL);
		pthread_attr_init(&attr);
		if((ret = pthread_create(&work_tid, &attr, dispose, NULL)) != 0){
			fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
			exit(1);
		}
#endif
		/* Add connections to the tested server for exchanging info */
		mappings = clt_settings.transfer.mappings;
		for(i = 0; i < clt_settings.transfer.num; i++){
			pair = mappings[i];
			online_port = pair->online_port;
			target_ip   = pair->target_ip;
			target_port = pair->target_port;
			address_add_msg_conn(online_port, target_ip, 
					clt_settings.srv_port);
			log_info(LOG_NOTICE,"add a tunnel for exchanging info:%u",
					ntohs(target_port));
		}
		return SUCCESS;
	}else
	{
		return FAILURE;
	}

}

