#include <errno.h>
#include <string.h>
#include <gromox/defs.h>
#include "cache_queue.h"
#include "gateway_dispatch.h"
#include "smtp_dispatch.h"
#include "files_allocator.h"
#include "net_failure.h"
#include "bounce_producer.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define MAX_CIRCLE_NUMBER   0x7FFFFFFF
#define DEF_MODE            S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

static char g_path[256];
static int g_mess_id;
static int g_scan_interval;
static int g_retrying_times;
static pthread_t g_thread_id;
static pthread_mutex_t g_id_lock;
static BOOL g_notify_stop = TRUE;

static int cache_queue_retrieve_mess_ID(void);
static int cache_queue_increase_mess_ID(void);
static void* thread_work_func(void* arg);

/*
 *	timer queue's construct function
 *	@param
 *		path [in]				queue path
 *		scan_interval			interval of queue scanning
 *		retrying_times			retrying times of delivery
 */
void cache_queue_init(const char *path, int scan_interval, int retrying_times)
{
	strcpy(g_path, path);
	g_scan_interval = scan_interval;
	g_retrying_times = retrying_times;
	g_notify_stop = TRUE;
	pthread_mutex_init(&g_id_lock, NULL);
}

/*
 *	run the timer queue module
 *	@return
 *		 0				OK
 *		<>0				fail
 */
int cache_queue_run()
{
	pthread_attr_t  attr;
	struct stat node_stat;

	/* check the directory */
	if (0 != stat(g_path, &node_stat)) {
		printf("[gateway_dispatch]: can not find %s directory\n", g_path);
		return -1;
	}
	if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("[gateway_dispatch]: %s is not a directory\n", g_path);
		return -2;
	}
	g_mess_id = cache_queue_retrieve_mess_ID();
	g_notify_stop = FALSE;
	pthread_attr_init(&attr);
	int ret = pthread_create(&g_thread_id, &attr, thread_work_func, nullptr);
	if (ret != 0) {
		pthread_attr_destroy(&attr);
		g_notify_stop = TRUE;
		printf("[gateway_dispatch]: failed to create timer thread: %s\n", strerror(ret));
		return -3;
	}
	pthread_setname_np(g_thread_id, "gwdispatch/cq");
	pthread_attr_destroy(&attr);
	return 0;
}

/*
 *	stop the timer queue module
 *	@return
 *		 0					OK
 *		<>0					fail
 */
void cache_queue_stop()
{
	if (FALSE == g_notify_stop) {
		g_notify_stop = TRUE;
		pthread_join(g_thread_id, NULL);
	}
}

/*
 *	timer queue's destruct function
 */
void cache_queue_free()
{
	g_path[0] = '\0';
	g_scan_interval = 0;
	g_retrying_times = 0;
	pthread_mutex_destroy(&g_id_lock);
}

/*
 *	put message into timer queue
 *	@param
 *		pcontext [in]		message context to be sent
 *		original_time		original time
 *	@return
 *		>=0					timer ID
 *		<0					fail
 */
int cache_queue_put(MESSAGE_CONTEXT *pcontext, time_t original_time)
{
	int mess_id, len, temp_len;
	char file_name[256];
	char tmp_buff[256];
	int fd, times;

	mess_id = cache_queue_increase_mess_ID();
	sprintf(file_name, "%s/%d", g_path, mess_id);
	fd = open(file_name, O_WRONLY|O_CREAT|O_TRUNC, DEF_MODE);
	if (-1 == fd) {
		return -1;
	}
	/* write 0 at the begin of file to indicate message is now being writing */
	times = 0;
	if (sizeof(int) != write(fd, &times, sizeof(int)) ||
		sizeof(time_t) != write(fd, &original_time, sizeof(time_t))) {
		close(fd);
		remove(file_name);
		return -1;
	}
	/* at the begin of file, write the length of message */
	len = mail_get_length(pcontext->pmail);
	if (-1 == len) {
		printf("[gateway_dispatch]: fail to get mail length\n");
		close(fd);
		remove(file_name);
		return -1;
	}
	if (sizeof(int) != write(fd, &len, sizeof(int))) {
		close(fd);
		remove(file_name);
		return -1;
	}
	if (FALSE == mail_to_file(pcontext->pmail, fd) ||
		sizeof(int) != write(fd, &pcontext->pcontrol->queue_ID, sizeof(int)) ||
		sizeof(int) != write(fd, &pcontext->pcontrol->bound_type, sizeof(int))||
		sizeof(BOOL) != write(fd, &pcontext->pcontrol->is_spam, sizeof(BOOL))||
		sizeof(BOOL) != write(fd, &pcontext->pcontrol->need_bounce,
		sizeof(BOOL))) {
		close(fd);
		remove(file_name);
		return -1;
	}
	/* write envelop from */
	temp_len = strlen(pcontext->pcontrol->from);
	temp_len ++;
	if (temp_len != write(fd, pcontext->pcontrol->from, temp_len)) {
		close(fd);
		remove(file_name);
		return -1;
	}
	/* write envelop rcpt */
	mem_file_seek(&pcontext->pcontrol->f_rcpt_to, MEM_FILE_READ_PTR, 0,
			MEM_FILE_SEEK_BEGIN);
	while (MEM_END_OF_FILE != (temp_len = mem_file_readline(
		&pcontext->pcontrol->f_rcpt_to, tmp_buff, 256))) {
		tmp_buff[temp_len] = '\0';
		temp_len ++;
		if (temp_len != write(fd, tmp_buff, temp_len)) {
			close(fd);
			remove(file_name);
			return -1;
		}
	}
	/* last null character for indicating end of rcpt to array */
	*tmp_buff = 0;
	if (1 != write(fd, tmp_buff, 1)) {
		close(fd);
		remove(file_name);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);
	times = 1;
	if (sizeof(int) != write(fd, &times, sizeof(int))) {
		close(fd);
		remove(file_name);
		return -1;
	}
	close(fd);
	return mess_id;
}

/*
 *	retrieve the message ID in the queue
 *	@return
 *		message ID
 */
static int cache_queue_retrieve_mess_ID()
{
	DIR *dirp;
	struct dirent *direntp;
	int max_ID = 0, temp_ID;

	/*
	read every file under directory and retrieve the maximum number and
	return it
	*/
	dirp = opendir(g_path);
	while ((direntp = readdir(dirp)) != NULL) {
		if (strcmp(direntp->d_name, ".") == 0 ||
		    strcmp(direntp->d_name, "..") == 0)
			continue;
		temp_ID = atoi(direntp->d_name);
		if (temp_ID > max_ID) {
			max_ID = temp_ID;
		}
	}
	closedir(dirp);
	return max_ID;
}

/*
 *	increase the message ID with 1
 *	@return
 *		message ID before increasement
 */
static int cache_queue_increase_mess_ID()
{
	int current_id;
	pthread_mutex_lock(&g_id_lock);
	if (MAX_CIRCLE_NUMBER == g_mess_id) {
		g_mess_id = 1;
	} else {
		g_mess_id ++;
	}
	current_id  = g_mess_id;
	pthread_mutex_unlock(&g_id_lock);
	return current_id;
}

static void* thread_work_func(void* arg)
{
	DIR *dirp;
	MIME *pmime;
	char ip_addr[16];
	struct stat node_stat;
	struct dirent *direntp;
	BOOL need_bounce, need_remove;
	int scan_interval, fd, mess_len;
	int i, times, size, len, bounce_type;
	char temp_path[256], reason_buff[1024];
	time_t scan_begin, scan_end, original_time;
	char *pbuff, *ptr, host_ip[16], *from_domain;
	MESSAGE_CONTEXT *pcontext, *pbounce_context;
	

	pcontext = get_context();
	if (NULL == pcontext) {
		printf("[gateway_dispatch]: fail to get context in cache queue thread\n");
		pthread_exit(0);
	}
	dirp = opendir(g_path);
	if (NULL == dirp) {
		printf("[gateway_dispatch]: failed to open cache directory %s: %s\n",
			g_path, strerror(errno));
		pthread_exit(0);
	}
	i = 0;
	scan_interval = g_scan_interval;
	while (FALSE == g_notify_stop) {
		if (i < scan_interval) {
			i ++;
			sleep(1);
			continue;
		}
		seekdir(dirp, 0);
		time(&scan_begin);
		while ((direntp = readdir(dirp)) != NULL) {
			if (FALSE != g_notify_stop) {
				break;
			}
			if (strcmp(direntp->d_name, ".") == 0 ||
			    strcmp(direntp->d_name, "..") == 0)
				continue;
			sprintf(temp_path, "%s/%s", g_path, direntp->d_name);
			if (0 != stat(temp_path, &node_stat) ||
				0 == S_ISREG(node_stat.st_mode)) {
				continue;
			}
			fd = open(temp_path, O_RDWR);
			if (-1 == fd) {
				continue;
			}
			if (sizeof(int) != read(fd, &times, sizeof(int))) {
				close(fd);
				continue;
			}
			if (0 == times) {
				close(fd);
				continue;
			}
			if (sizeof(time_t) != read(fd, &original_time, sizeof(time_t)) ||
				sizeof(int) != read(fd, &mess_len, sizeof(int))) {
				printf("[gateway_dispatch]: fail to read information from %s "
					"in timer queue\n", direntp->d_name);
				close(fd);
				continue;
			}
			size = node_stat.st_size - sizeof(time_t) - 2*sizeof(int);
			pbuff = malloc(((size - 1)/(64 * 1024) + 1) * 64 * 1024);
			if (NULL == pbuff) {
				printf("[gateway_dispatch]: Failed to allocate memory for %s "
					"in timer queue thread\n", direntp->d_name);
				close(fd);
				continue;
			}
			if (size != read(fd, pbuff, size)) {
				free(pbuff);
				printf("[gateway_dispatch]: fail to read information from %s "
					"in timer queue\n", direntp->d_name);
				close(fd);
				continue;
			}
			if (FALSE == mail_retrieve(pcontext->pmail, pbuff, mess_len)) {
				free(pbuff);
				printf("[gateway_dispatch]: fail to retrieve message %s in "
					"cache queue into mail object\n", direntp->d_name);
				close(fd);
				continue;
			}
			ptr = pbuff + mess_len;
			pcontext->pcontrol->queue_ID = *(int*)ptr;
			ptr += sizeof(int);
			pcontext->pcontrol->bound_type = *(int*)ptr;
			ptr += sizeof(int);
			pcontext->pcontrol->is_spam = *(BOOL*)ptr;
			ptr += sizeof(BOOL);
			pcontext->pcontrol->need_bounce = *(BOOL*)ptr;
			ptr += sizeof(BOOL);
			strcpy(pcontext->pcontrol->from, ptr);
			ptr += strlen(pcontext->pcontrol->from) + 1;
			mem_file_clear(&pcontext->pcontrol->f_rcpt_to);
			while ((len = strlen(ptr)) != 0) {
				mem_file_writeline(&pcontext->pcontrol->f_rcpt_to, ptr);
				ptr += len + 1;
			}
			if (g_retrying_times <= times) {
				need_bounce = TRUE;
				need_remove = TRUE;
				bounce_type = BOUNCE_RESPONSE_ERROR;
			} else {
				need_bounce = FALSE;
				need_remove = FALSE;
			}
			switch (smtp_dispatch_process(pcontext,
				ip_addr, reason_buff, 1024)) {
			case SMTP_DISPATCH_OK:
				need_bounce = FALSE;
				need_remove = TRUE;
				net_failure_statistic(1, 0, 0, 0);
				break;
			case SMTP_DISPATCH_TEMP_ERROR:
				net_failure_statistic(0, 1, 0, 0);
				break;
			case SMTP_DISPATCH_NO_USER:
			    bounce_type = BOUNCE_NO_USER;
			    need_bounce = TRUE;
				need_remove = TRUE;
			    net_failure_statistic(0, 0, 0, 1);
				/* ignore functionality of anti-enumeration ! */
				break;
			case SMTP_DISPATCH_PERMANENT_ERROR:
				bounce_type = BOUNCE_RESPONSE_ERROR;
			    need_bounce = TRUE;
				need_remove = TRUE;
			    net_failure_statistic(0, 0, 1, 0);
			    break;
			}
			if (FALSE == need_remove) {
				/* rewite type and until time */
				lseek(fd, 0, SEEK_SET);
				times ++;
				if (sizeof(int) != write(fd, &times, sizeof(int))) {
					printf("[gateway_dispatch]: error while updating "
						"times\n");
				}
			}
			close(fd);
			fd = -1;
			if (TRUE == need_remove) {
				remove(temp_path);
			}
			need_bounce &= pcontext->pcontrol->need_bounce;
			if (TRUE == need_bounce &&
				BOUND_IN == pcontext->pcontrol->bound_type) {
				switch (gateway_dispatch_get_param(
					GATEWAY_DISPATCH_BOUNCE_POLICY)) {
				case BOUNCE_POLICY_NONE:
					need_bounce = FALSE;
					break;
				case BOUNCE_POLICY_VERIFY:
					/* compare the B-class ip, if same, do not insulate the message */
					memset(host_ip, 0, 16);
					pmime = mail_get_head(pcontext->pmail);
					if (FALSE == mime_get_field(pmime, "X-Lasthop",
						host_ip, 16)) {
						need_bounce = FALSE;
					} else {
						from_domain = strchr(pcontext->pcontrol->from, '@') + 1;
						if (FALSE == gateway_dispatch_verify_ipdomain(
							from_domain, host_ip)) {
							need_bounce = FALSE;
							smtp_dispatch_log_info(pcontext, 8,
								"original ipaddr is different from DNS result, "
								"will not bounce nouser mail");
						}
					}
					break;
				}
			}
			
			/* no functionality of comparing B-class ip */
			if (TRUE == need_bounce) {
				pbounce_context = get_context();
				if (NULL == pbounce_context) {
					smtp_dispatch_log_info(pcontext, 8, "fail to get one "
						"context for bounce mail");
				} else {
					bounce_producer_make(pcontext, original_time, bounce_type,
						ip_addr, reason_buff, pbounce_context->pmail);
					sprintf(pbounce_context->pcontrol->from, "postmaster@%s",
						get_default_domain());
					mem_file_writeline(&pbounce_context->pcontrol->f_rcpt_to,
						pcontext->pcontrol->from);
					enqueue_context(pbounce_context);
				}
			}
			free(pbuff);
		}
		time(&scan_end);
		if (scan_end - scan_begin >= g_scan_interval) {
			scan_interval = 0;
		} else {
			scan_interval = g_scan_interval - (scan_end - scan_begin);
		}
		i = 0;
	}
	closedir(dirp);
	return NULL;
}

int cache_queue_get_param(int param)
{
	if (CACHE_QUEUE_SCAN_INTERVAL == param) {
		return g_scan_interval;
	} else if (CACHE_QUEUE_RETRYING_TIMES == param) {
		return g_retrying_times;
	}
	return 0;
}

void cache_queue_set_param(int param, int val)
{
	if (CACHE_QUEUE_SCAN_INTERVAL == param) {
		g_scan_interval = val;
	} else if (CACHE_QUEUE_RETRYING_TIMES == param) {
		g_retrying_times = val;
	}
}

