#include "httpd.h"
#include "http_log.h"
#include "http_config.h"

#include <time.h>
#include <unistd.h>
#include <sys/times.h>

#include <apr_strings.h>

/* Forward module declaration (why?) */
module AP_MODULE_DECLARE_DATA accounting_module;

/* Struct that contains the (begin) reference values */
typedef struct {
	struct timeval begin_time;
	struct rusage  begin_own_usage;
	struct rusage  begin_child_usage;
	
	/* Because we put this struct directly in the r->notes (***DIRTY***)
	 * we explicitly make sure it's zero terminated (although it's very
	 * likely the message already contains some zero bytes. */
	char           zero;
} acc_data;

/* Key values for use in the request_rec->notes table */
static const char *notes_key_internal = "ACC_INTERNAL";
static const char     *notes_key_time = "ACC_time";
static const char    *notes_key_utime = "ACC_utime";
static const char    *notes_key_stime = "ACC_stime";
static const char   *notes_key_cutime = "ACC_cutime";
static const char   *notes_key_cstime = "ACC_cstime";
static const char  *notes_key_inblock = "ACC_inblock";
static const char  *notes_key_oublock = "ACC_oublock";
static const char *notes_key_cinblock = "ACC_cinblock";
static const char *notes_key_coublock = "ACC_coublock";

/* Some defines that make the logging more readable */
#define ACC_LOG_REQ_ERROR(errmsg) \
	ap_log_error(        \
		APLOG_MARK,  \
		APLOG_ERR,   \
                APR_SUCCESS, \
		r->server,   \
		errmsg       \
	)

#define DEBUG 0

/* Wether or not the DEBUG macro's should do something */
#ifdef DEBUG
 #define ACC_LOG_DEBUG_TIME(msg, time) \
	ap_log_error(                      \
		APLOG_MARK,                \
		APLOG_NOERRNO|APLOG_DEBUG, \
		APR_SUCCESS,               \
		r->server,                 \
		"%s: %ld.%.6ldsec.",       \
		msg,                       \
		time.tv_sec,               \
		(long int) time.tv_usec    \
	)
 #define ACC_LOG_DEBUG_BLOCKS(msg, num)    \
	ap_log_error(                      \
		APLOG_MARK,                \
		APLOG_NOERRNO|APLOG_DEBUG, \
		APR_SUCCESS,               \
		r->server,                 \
		"%s: %ld",                 \
		msg,                       \
		num                        \
	)
#else
 #define ACC_LOG_DEBUG_TIME(msg, time) 	/* msg , time */
 #define ACC_LOG_DEBUG_BLOCKS(msg, num) /* msg , num  */
#endif

/* Calculate the time difference between begin and end
 *
 * This function determines the time difference between two different
 * "struct timeval" times. It has an added check to see if the values
 * describe a forward linear progression, otherwise we're timetravelling.
 *
 * When timetraveling messages are logged, make sure the code supplies the
 * (correct) values in the correct order (begin, end). If this is all correct
 * your system is probably broken.
 */
static long int time_difference(const request_rec *r, const struct timeval *begin, const struct timeval *end){ // {{{
	long long int retval;

	if (	/* Sanity check of values */
			end->tv_sec < begin->tv_sec ||
			(
				end->tv_sec == begin->tv_sec &&
				end->tv_usec < begin->tv_usec
			)
	   )
	{
		/* We traveled back in time?!?*/
		ap_log_error(
			APLOG_MARK,
			APLOG_ERR,
                        APR_SUCCESS,
			r->server,
			"Timetraveling: begin(%ld.%.6ldsec.) end(%ld.%.6ldsec.)",
			begin->tv_sec,
			(long int) begin->tv_usec,
			end->tv_sec,
			(long int) end->tv_usec
		);
		return 0;
	}

	/* Debug */ // {{{
	ACC_LOG_DEBUG_TIME(
		"time_difference:begin",
		(*begin)
	);
	ACC_LOG_DEBUG_TIME(
		"time_difference:end",
		(*end)
	); // }}}

	/* Calculate the time difference */
	retval = end->tv_sec - begin->tv_sec;
	retval *= 1000000;
	retval += end->tv_usec - begin->tv_usec;

	/* Return the difference */
	return retval;
} // }}}

/* Calculate the difference between begin and end values
 *
 * This function determines the difference between two normal getrusage
 * fields. It's is used only for in- and oublock fields. Negative values 
 * should not occur!
 */
static long block_difference(const request_rec *r, long begin, long end){ // {{{
	long retval;

	/* Sanity check of values */
	if (begin > end)
	{
		/* Difference is negative, shouldn't occur! */
		ap_log_error(
			APLOG_MARK,
			APLOG_ERR,
                        APR_SUCCESS,
			r->server,
			"Negative blockcount: begin(%ld blocks) end(%ld blocks)",
			begin,
			end
		);
		return 0;
	}

	/* Debug */ // {{{
	ACC_LOG_DEBUG_BLOCKS(
		"block_difference:begin",
		begin
	);
	ACC_LOG_DEBUG_BLOCKS(
		"block_difference:end",
		end
	); // }}}

	/* Calculate the time difference */
	retval = end - begin;

	/* Return the difference */
	return retval;
} // }}}



/* Start accounting
 *
 * Here we'll retrieve the reference (begin) values that are needed
 * to determine the resource usage of this request.
 */
static int module_accounting_start (request_rec *r){ // {{{
	/* printf("Module accounting start\n"); */
	acc_data *data;

	/* Determine the main request */
	request_rec *initial = r;
	while (initial->main)
		initial = initial->main;
	
	/* Determine the first request */
	while (initial->prev)
		initial = initial->prev;

	/* Check if we've already got reference (begin) timings */
	if (apr_table_get(initial->notes, notes_key_internal) != NULL)
	{
		/* We already set some reference (begin) values */
		return DECLINED;
	}
	
	/* Allocate internal message */
	data = (acc_data*) apr_palloc(initial->pool, sizeof(acc_data));

	/* What's the time since epoch? */
	if (gettimeofday(&(data->begin_time), NULL) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for (begin) time of day failed");
	}

	/* Get the accumelated resource usage of this process */
	if (getrusage(RUSAGE_SELF, &(data->begin_own_usage)) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for (begin) resource usage failed");
	}

	/* Get the accumelated resource usage for childeren of this process */
	if (getrusage(RUSAGE_CHILDREN, &(data->begin_child_usage)) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for children's (begin) resource usage failed");
	}

	/* Add zero padding to structure */
	data->zero = '\0';

	/* Debug */ // {{{
	ACC_LOG_DEBUG_TIME(
		"accounting_start:data->begin_time",
		data->begin_time
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_start:data->begin_own_usage.ru_utime",
		data->begin_own_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_start:data->begin_own_usage.ru_stime",
		data->begin_own_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_start:data->begin_own_usage.ru_inblock",
		data->begin_own_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_start:data->begin_own_usage.ru_oublock",
		data->begin_own_usage.ru_oublock
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_start:data->begin_child_usage.ru_utime",
		data->begin_child_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_start:data->begin_child_usage.ru_stime",
		data->begin_child_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_start:data->begin_child_usage.ru_inblock",
		data->begin_child_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_start:data->begin_child_usage.ru_oublock",
		data->begin_child_usage.ru_oublock
	); // }}}

	/* Add this data as to the notes field of request_rec */
	apr_table_setn(initial->notes, notes_key_internal, (char*) data);

	/* We're not a final handler */
	return DECLINED;
} // }}}


/* Stop accounting
 *
 * Here we will request the resource information at the end of the period
 * we want to account. Using the reference (begin) values we retrieved in
 * the module_acccounting_start() function we can now determine the resource
 * usage of this request.
 */
static int module_accounting_stop(request_rec *r) { // {{{
	/* printf("Module accounting stop\n"); */
	struct timeval end_time;
	struct rusage  end_own_usage;
	struct rusage  end_child_usage;
	acc_data *data;
	
	/* Resolve the internal redirect request */
	request_rec *initial;
	request_rec *last;
	
	/* Determine the main request */
	initial = r;
	while (initial->main)
		initial = initial->main;
	last = initial;

	/* Determine the first request */
	while (initial->prev)
		initial = initial->prev;

	/* Determine the last request */
	while (last->next)
		last = last->next;
	
	/* Get the reference (begin) data */
	if ((data = (acc_data*) apr_table_get(initial->notes, notes_key_internal)) == NULL)
	{
		/* Internal data missing ?!? */
		ACC_LOG_REQ_ERROR("Failed to fetch internal data!");

		return DECLINED;
	}
	
	/* Request resource information at this point */

	/* Try waiting for children */
	wait4(-1, NULL, WNOHANG, NULL);

	/* What's the time since epoch? */
	if (gettimeofday(&(end_time), NULL) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for (end) time of day failed");
	}

	/* Get the accumelated resource usage of this process */
	if (getrusage(RUSAGE_SELF, &(end_own_usage)) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for (end) resource usage failed");
	}

	/* Get the accumelated resource usage for childeren of this process */
	if (getrusage(RUSAGE_CHILDREN, &(end_child_usage)) == -1)
	{
		/* ERROR */
		ACC_LOG_REQ_ERROR("Request for children's (end) resource usage failed");
	}

	/* Debug */ // {{{
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:data->begin_time",
		data->begin_time
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:data->begin_own_usage.ru_utime",
		data->begin_own_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:data->begin_own_usage.ru_stime",
		data->begin_own_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:data->begin_own_usage.ru_inblock",
		data->begin_own_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:data->begin_own_usage.ru_oublock",
		data->begin_own_usage.ru_oublock
	);

	ACC_LOG_DEBUG_TIME(
		"accounting_stop:data->begin_child_usage.ru_utime",
		data->begin_child_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:data->begin_child_usage.ru_stime",
		data->begin_child_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:data->begin_child_usage.ru_inblock",
		data->begin_child_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:data->begin_child_usage.ru_oublock",
		data->begin_child_usage.ru_oublock
	);

	ACC_LOG_DEBUG_TIME(
		"accounting_stop:end_time",
		end_time
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:end_own_usage.ru_utime",
		end_own_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:end_own_usage.ru_stime",
		end_own_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:end_own_usage.ru_inblock",
		end_own_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:end_own_usage.ru_oublock",
		end_own_usage.ru_oublock
	);

	ACC_LOG_DEBUG_TIME(
		"accounting_stop:end_child_usage.ru_utime",
		end_child_usage.ru_utime
	);
	ACC_LOG_DEBUG_TIME(
		"accounting_stop:end_child_usage.ru_stime",
		end_child_usage.ru_stime
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:end_child_usage.ru_inblock",
		end_child_usage.ru_inblock
	);
	ACC_LOG_DEBUG_BLOCKS(
		"accounting_stop:end_child_usage.ru_oublock",
		end_child_usage.ru_oublock
	); // }}}
	
	/* Set the time difference between start and stop */
	apr_table_setn(
		last->notes,
		notes_key_time,
		apr_psprintf(
			last->pool,
			"%ld",
			time_difference(
				last,
				&(data->begin_time),
				&(end_time)
			)
		)
	);
	
	/* Set the accumilated user time */
	apr_table_setn( 
		last->notes,
		notes_key_utime,
		apr_psprintf(
			last->pool,
			"%ld",
			time_difference(
				last,
				&(data->begin_own_usage.ru_utime),
				&(end_own_usage.ru_utime)
			)
		)
	);
	
	/* Set the accumilated system time */
	apr_table_setn(
		last->notes,
		notes_key_stime,
		apr_psprintf(
			last->pool,
			"%ld",
			time_difference(
				last,
				&(data->begin_own_usage.ru_stime),
				&(end_own_usage.ru_stime)
			)
		)
	);
	/* 	
	apr_table_setn(
		last->notes,
		notes_key_stime,
		apr_psprintf(
			last->pool,
			"%ld",
			end_own_usage.ru_stime.tv_usec
		)
	); */
	
	/* Set the accumulated inblocks */
	apr_table_setn( 
		last->notes,
		notes_key_inblock,
		apr_psprintf(
			last->pool,
			"%ld",
			block_difference(
				last,
				data->begin_own_usage.ru_inblock,
				end_own_usage.ru_inblock
			)
		)
	);
	
	/* Set the accumulated oublocks */
	apr_table_setn( 
		last->notes,
		notes_key_oublock,
		apr_psprintf(
			last->pool,
			"%ld",
			block_difference(
				last,
				data->begin_own_usage.ru_oublock,
				end_own_usage.ru_oublock
			)
		)
	);
	
	/* Set the child accumilated user time */
	apr_table_setn(
		last->notes,
		notes_key_cutime,
		apr_psprintf(
			last->pool,
			"%ld",
			time_difference(
				last,
				&(data->begin_child_usage.ru_utime),
				&(end_child_usage.ru_utime)
			)
		)
	);
	
	/* Set the child accumilated system time */
	apr_table_setn(
		last->notes,
		notes_key_cstime,
		apr_psprintf(
			last->pool,
			"%ld",
			time_difference(
				last,
				&(data->begin_child_usage.ru_stime),
				&(end_child_usage.ru_stime)
			)
		)
	);
	
	/* Set the child accumulated inblocks */
	apr_table_setn(
		last->notes,
		notes_key_cinblock,
		apr_psprintf(
			last->pool,
			"%ld",
			block_difference(
				last,
				data->begin_child_usage.ru_inblock,
				end_child_usage.ru_inblock
			)
		)
	);
	
	/* Set the child accumulated oublocks */
	apr_table_setn(
		last->notes,
		notes_key_coublock,
		apr_psprintf(
			last->pool,
			"%ld",
			block_difference(
				last,
				data->begin_child_usage.ru_oublock,
				end_child_usage.ru_oublock
			)
		)
	);

	/* We're not a final handler */
    return DECLINED;
} // }}}


static void register_hooks(apr_pool_t *p){ // {{{
   ap_hook_post_read_request(module_accounting_start, NULL, NULL, APR_HOOK_MIDDLE);
   ap_hook_log_transaction(module_accounting_stop, NULL, NULL, APR_HOOK_FIRST);
} // }}}


module AP_MODULE_DECLARE_DATA accounting_module = { // {{{
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-dir config */
    NULL,                       /* merge per-dir config */
    NULL,                       /* server config */
    NULL,                       /* merge server config */
    NULL,                       /* command table */
    register_hooks
}; // }}}

