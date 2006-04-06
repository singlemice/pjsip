/* $Id$ */
/* 
 * Copyright (C) 2003-2006 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pjmedia/session.h>
#include <pjmedia/errno.h>
#include <pj/log.h>
#include <pj/os.h> 
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/assert.h>
#include <pj/ctype.h>


struct pjmedia_session
{
    pj_pool_t		   *pool;
    pjmedia_endpt	   *endpt;
    unsigned		    stream_cnt;
    pjmedia_stream_info	    stream_info[PJMEDIA_MAX_SDP_MEDIA];
    pjmedia_stream	   *stream[PJMEDIA_MAX_SDP_MEDIA];
    void		   *user_data;
};

#define THIS_FILE		"session.c"

#define PJMEDIA_SESSION_SIZE	(48*1024)
#define PJMEDIA_SESSION_INC	1024

static const pj_str_t ID_AUDIO = { "audio", 5};
static const pj_str_t ID_VIDEO = { "video", 5};
static const pj_str_t ID_IN = { "IN", 2 };
static const pj_str_t ID_IP4 = { "IP4", 3};
static const pj_str_t ID_RTP_AVP = { "RTP/AVP", 7 };
static const pj_str_t ID_SDP_NAME = { "pjmedia", 7 };
static const pj_str_t ID_RTPMAP = { "rtpmap", 6 };
static const pj_str_t ID_TELEPHONE_EVENT = { "telephone-event", 15 };

static const pj_str_t STR_INACTIVE = { "inactive", 8 };
static const pj_str_t STR_SENDRECV = { "sendrecv", 8 };
static const pj_str_t STR_SENDONLY = { "sendonly", 8 };
static const pj_str_t STR_RECVONLY = { "recvonly", 8 };


/*
 * Create stream info from SDP media line.
 */
PJ_DEF(pj_status_t) pjmedia_stream_info_from_sdp(
					   pjmedia_stream_info *si,
					   pj_pool_t *pool,
					   pjmedia_endpt *endpt,
					   const pjmedia_sdp_session *local,
					   const pjmedia_sdp_session *remote,
					   unsigned stream_idx)
{
    const pjmedia_sdp_attr *attr;
    const pjmedia_sdp_media *local_m;
    const pjmedia_sdp_media *rem_m;
    const pjmedia_sdp_conn *local_conn;
    const pjmedia_sdp_conn *rem_conn;
    pjmedia_sdp_rtpmap *rtpmap;
    unsigned i, pt;
    pj_status_t status;


    
    /* Validate arguments: */
    PJ_ASSERT_RETURN(pool && si && local && remote, PJ_EINVAL);
    PJ_ASSERT_RETURN(stream_idx < local->media_count, PJ_EINVAL);
    PJ_ASSERT_RETURN(stream_idx < remote->media_count, PJ_EINVAL);


    local_m = local->media[stream_idx];
    rem_m = remote->media[stream_idx];

    local_conn = local_m->conn ? local_m->conn : local->conn;
    if (local_conn == NULL)
	return PJMEDIA_SDP_EMISSINGCONN;

    rem_conn = rem_m->conn ? rem_m->conn : remote->conn;
    if (rem_conn == NULL)
	return PJMEDIA_SDP_EMISSINGCONN;


    /* Reset: */

    pj_memset(si, 0, sizeof(*si));

    /* Media type: */

    if (pj_stricmp(&local_m->desc.media, &ID_AUDIO) == 0) {

	si->type = PJMEDIA_TYPE_AUDIO;

    } else if (pj_stricmp(&local_m->desc.media, &ID_VIDEO) == 0) {

	si->type = PJMEDIA_TYPE_VIDEO;

    } else {

	si->type = PJMEDIA_TYPE_UNKNOWN;
	return PJMEDIA_EINVALIMEDIATYPE;
    }

    /* Media direction: */

    if (local_m->desc.port == 0 || 
	pj_inet_addr(&local_conn->addr).s_addr==0 ||
	pj_inet_addr(&rem_conn->addr).s_addr==0 ||
	pjmedia_sdp_media_find_attr(local_m, &STR_INACTIVE, NULL)!=NULL)
    {
	/* Inactive stream. */

	si->dir = PJMEDIA_DIR_NONE;

    } else if (pjmedia_sdp_media_find_attr(local_m, &STR_SENDONLY, NULL)!=NULL) {

	/* Send only stream. */

	si->dir = PJMEDIA_DIR_ENCODING;

    } else if (pjmedia_sdp_media_find_attr(local_m, &STR_RECVONLY, NULL)!=NULL) {

	/* Recv only stream. */

	si->dir = PJMEDIA_DIR_DECODING;

    } else {

	/* Send and receive stream. */

	si->dir = PJMEDIA_DIR_ENCODING_DECODING;

    }


    /* Set remote address: */

    si->rem_addr.sin_family = PJ_AF_INET;
    si->rem_addr.sin_port = pj_htons(rem_m->desc.port);
    if (pj_inet_aton(&rem_conn->addr, &si->rem_addr.sin_addr) == 0) {

	/* Invalid IP address. */
	return PJMEDIA_EINVALIDIP;
    }

    /* And codec must be numeric! */
    if (!pj_isdigit(*local_m->desc.fmt[0].ptr) || 
	!pj_isdigit(*rem_m->desc.fmt[0].ptr))
    {
	return PJMEDIA_EINVALIDPT;
    }

    /* Get the payload number for receive channel. */
    pt = pj_strtoul(&local_m->desc.fmt[0]);

    /* Get codec info.
     * For static payload types, get the info from codec manager.
     * For dynamic payload types, MUST get the rtpmap.
     */
    if (pt < 96) {
	pj_bool_t has_rtpmap;

	rtpmap = NULL;
	has_rtpmap = PJ_TRUE;

	attr = pjmedia_sdp_media_find_attr(local_m, &ID_RTPMAP, 
					   &local_m->desc.fmt[0]);
	if (attr == NULL) {
	    has_rtpmap = PJ_FALSE;
	}
	if (attr != NULL) {
	    status = pjmedia_sdp_attr_to_rtpmap(pool, attr, &rtpmap);
	    if (status != PJ_SUCCESS)
		has_rtpmap = PJ_FALSE;
	}

	/* Build codec format info: */
	if (has_rtpmap) {
	    si->fmt.type = si->type;
	    si->fmt.pt = pj_strtoul(&local_m->desc.fmt[0]);
	    pj_strdup(pool, &si->fmt.encoding_name, &rtpmap->enc_name);
	    si->fmt.sample_rate = rtpmap->clock_rate;

	} else {
	    pjmedia_codec_mgr *mgr;

	    mgr = pjmedia_endpt_get_codec_mgr(endpt);

	    status = pjmedia_codec_mgr_get_codec_info( mgr, pt, &si->fmt);
	    if (status != PJ_SUCCESS)
		return status;
	}

	/* For static payload type, pt's are symetric */
	si->tx_pt = pt;

    } else {
	attr = pjmedia_sdp_media_find_attr(local_m, &ID_RTPMAP, 
					   &local_m->desc.fmt[0]);
	if (attr == NULL)
	    return PJMEDIA_EMISSINGRTPMAP;

	status = pjmedia_sdp_attr_to_rtpmap(pool, attr, &rtpmap);
	if (status != PJ_SUCCESS)
	    return status;

	/* Build codec format info: */

	si->fmt.type = si->type;
	si->fmt.pt = pj_strtoul(&local_m->desc.fmt[0]);
	pj_strdup(pool, &si->fmt.encoding_name, &rtpmap->enc_name);
	si->fmt.sample_rate = rtpmap->clock_rate;

	/* Determine payload type for outgoing channel, by finding
	 * dynamic payload type in remote SDP that matches the answer.
	 */
	si->tx_pt = 0xFFFF;
	for (i=0; i<rem_m->desc.fmt_count; ++i) {
	    unsigned rpt;
	    pjmedia_sdp_attr *r_attr;
	    pjmedia_sdp_rtpmap r_rtpmap;

	    rpt = pj_strtoul(&rem_m->desc.fmt[i]);
	    if (rpt < 96)
		continue;

	    r_attr = pjmedia_sdp_media_find_attr(rem_m, &ID_RTPMAP,
						 &rem_m->desc.fmt[i]);
	    if (!r_attr)
		continue;

	    if (pjmedia_sdp_attr_get_rtpmap(r_attr, &r_rtpmap) != PJ_SUCCESS)
		continue;

	    if (!pj_stricmp(&rtpmap->enc_name, &r_rtpmap.enc_name) &&
		rtpmap->clock_rate == r_rtpmap.clock_rate)
	    {
		/* Found matched codec. */
		si->tx_pt = rpt;
		break;
	    }
	}

	if (si->tx_pt == 0xFFFF)
	    return PJMEDIA_EMISSINGRTPMAP;
    }

  

    /* Get local DTMF payload type */
    si->tx_event_pt = -1;
    for (i=0; i<local_m->attr_count; ++i) {
	pjmedia_sdp_rtpmap r;

	attr = local_m->attr[i];
	if (pj_strcmp(&attr->name, &ID_RTPMAP) != 0)
	    continue;
	if (pjmedia_sdp_attr_get_rtpmap(attr, &r) != PJ_SUCCESS)
	    continue;
	if (pj_strcmp(&r.enc_name, &ID_TELEPHONE_EVENT) == 0) {
	    si->tx_event_pt = pj_strtoul(&r.pt);
	    break;
	}
    }

    /* Get remote DTMF payload type */
    si->rx_event_pt = -1;
    for (i=0; i<rem_m->attr_count; ++i) {
	pjmedia_sdp_rtpmap r;

	attr = rem_m->attr[i];
	if (pj_strcmp(&attr->name, &ID_RTPMAP) != 0)
	    continue;
	if (pjmedia_sdp_attr_get_rtpmap(attr, &r) != PJ_SUCCESS)
	    continue;
	if (pj_strcmp(&r.enc_name, &ID_TELEPHONE_EVENT) == 0) {
	    si->rx_event_pt = pj_strtoul(&r.pt);
	    break;
	}
    }


    /* Leave SSRC to zero. */

    /* Leave jitter buffer parameter. */
    
    return PJ_SUCCESS;
}


/**
 * Create new session.
 */
PJ_DEF(pj_status_t) pjmedia_session_create( pjmedia_endpt *endpt, 
					    unsigned stream_cnt,
					    const pjmedia_sock_info skinfo[],
					    const pjmedia_sdp_session *local_sdp,
					    const pjmedia_sdp_session *rem_sdp,
					    void *user_data,
					    pjmedia_session **p_session )
{
    pj_pool_t *pool;
    pjmedia_session *session;
    int i; /* Must be signed */
    pj_status_t status;

    /* Verify arguments. */
    PJ_ASSERT_RETURN(endpt && stream_cnt && skinfo &&
		     local_sdp && rem_sdp && p_session, PJ_EINVAL);

    /* Create pool for the session. */
    pool = pjmedia_endpt_create_pool( endpt, "session", 
				      PJMEDIA_SESSION_SIZE, 
				      PJMEDIA_SESSION_INC);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);

    session = pj_pool_zalloc(pool, sizeof(pjmedia_session));
    session->pool = pool;
    session->endpt = endpt;
    session->stream_cnt = stream_cnt;
    session->user_data = user_data;
    
    /* Stream count is the lower number of stream_cnt or SDP m= lines count */
    if (stream_cnt < local_sdp->media_count)
	stream_cnt = local_sdp->media_count;

    /* 
     * Create streams: 
     */
    for (i=0; i<(int)stream_cnt; ++i) {

	pjmedia_stream_info *si = &session->stream_info[i];

	/* Build stream info based on media line in local SDP */
	status = pjmedia_stream_info_from_sdp(si, session->pool, endpt,
					      local_sdp, rem_sdp, i);
	if (status != PJ_SUCCESS)
	    return status;

	/* Assign sockinfo */
	si->sock_info = skinfo[i];
    }

    /*
     * Now create and start the stream!
     */
    for (i=0; i<(int)stream_cnt; ++i) {

	status = pjmedia_stream_create(endpt, session->pool,
				       &session->stream_info[i],
				       session,
				       &session->stream[i]);
	if (status == PJ_SUCCESS)
	    status = pjmedia_stream_start(session->stream[i]);

	if (status != PJ_SUCCESS) {

	    for ( --i; i>=0; ++i) {
		pjmedia_stream_destroy(session->stream[i]);
	    }

	    pj_pool_release(session->pool);
	    return status;
	}
    }


    /* Done. */

    *p_session = session;
    return PJ_SUCCESS;
}


/*
 * Get session info.
 */
PJ_DEF(pj_status_t) pjmedia_session_get_info( pjmedia_session *session,
					      pjmedia_session_info *info )
{
    PJ_ASSERT_RETURN(session && info, PJ_EINVAL);

    info->stream_cnt = session->stream_cnt;
    pj_memcpy(info->stream_info, session->stream_info,
	      session->stream_cnt * sizeof(pjmedia_stream_info));

    return PJ_SUCCESS;
}


/**
 * Destroy media session.
 */
PJ_DEF(pj_status_t) pjmedia_session_destroy (pjmedia_session *session)
{
    unsigned i;

    PJ_ASSERT_RETURN(session, PJ_EINVAL);

    for (i=0; i<session->stream_cnt; ++i) {
	
	pjmedia_stream_destroy(session->stream[i]);

    }

    pj_pool_release (session->pool);

    return PJ_SUCCESS;
}


/**
 * Activate all stream in media session.
 *
 */
PJ_DEF(pj_status_t) pjmedia_session_resume(pjmedia_session *session,
					   pjmedia_dir dir)
{
    unsigned i;

    PJ_ASSERT_RETURN(session, PJ_EINVAL);

    for (i=0; i<session->stream_cnt; ++i) {
	pjmedia_session_resume_stream(session, i, dir);
    }

    return PJ_SUCCESS;
}


/**
 * Suspend receipt and transmission of all stream in media session.
 *
 */
PJ_DEF(pj_status_t) pjmedia_session_pause(pjmedia_session *session,
					  pjmedia_dir dir)
{
    unsigned i;

    PJ_ASSERT_RETURN(session, PJ_EINVAL);

    for (i=0; i<session->stream_cnt; ++i) {
	pjmedia_session_pause_stream(session, i, dir);
    }

    return PJ_SUCCESS;
}


/**
 * Suspend receipt and transmission of individual stream in media session.
 */
PJ_DEF(pj_status_t) pjmedia_session_pause_stream( pjmedia_session *session,
						  unsigned index,
						  pjmedia_dir dir)
{
    PJ_ASSERT_RETURN(session && index < session->stream_cnt, PJ_EINVAL);

    return pjmedia_stream_pause(session->stream[index], dir);
}


/**
 * Activate individual stream in media session.
 *
 */
PJ_DEF(pj_status_t) pjmedia_session_resume_stream( pjmedia_session *session,
						   unsigned index,
						   pjmedia_dir dir)
{
    PJ_ASSERT_RETURN(session && index < session->stream_cnt, PJ_EINVAL);

    return pjmedia_stream_resume(session->stream[index], dir);
}

/**
 * Enumerate media stream in the session.
 */
PJ_DEF(pj_status_t) pjmedia_session_enum_streams(const pjmedia_session *session,
						 unsigned *count, 
						 pjmedia_stream_info info[])
{
    unsigned i;

    PJ_ASSERT_RETURN(session && count && *count && info, PJ_EINVAL);

    if (*count > session->stream_cnt)
	*count = session->stream_cnt;

    for (i=0; i<*count; ++i) {
	pj_memcpy(&info[i], &session->stream[i], sizeof(pjmedia_stream_info));
    }

    return PJ_SUCCESS;
}


/*
 * Get the port interface.
 */
PJ_DEF(pj_status_t) pjmedia_session_get_port(  pjmedia_session *session,
					       unsigned index,
					       pjmedia_port **p_port)
{
    return pjmedia_stream_get_port( session->stream[index], p_port);
}

/*
 * Get statistics
 */
PJ_DEF(pj_status_t) pjmedia_session_get_stream_stat( pjmedia_session *session,
						     unsigned index,
						     pjmedia_rtcp_stat *stat)
{
    PJ_ASSERT_RETURN(session && stat && index < session->stream_cnt, 
		     PJ_EINVAL);

    return pjmedia_stream_get_stat(session->stream[index], stat);
}


/*
 * Dial DTMF digit to the stream, using RFC 2833 mechanism.
 */
PJ_DEF(pj_status_t) pjmedia_session_dial_dtmf( pjmedia_session *session,
					       unsigned index,
					       const pj_str_t *ascii_digits )
{
    PJ_ASSERT_RETURN(session && ascii_digits, PJ_EINVAL);
    return pjmedia_stream_dial_dtmf(session->stream[index], ascii_digits);
}

/*
 * Check if the specified stream has received DTMF digits.
 */
PJ_DEF(pj_status_t) pjmedia_session_check_dtmf( pjmedia_session *session,
					        unsigned index )
{
    PJ_ASSERT_RETURN(session, PJ_EINVAL);
    return pjmedia_stream_check_dtmf(session->stream[index]);
}


/*
 * Retrieve DTMF digits from the specified stream.
 */
PJ_DEF(pj_status_t) pjmedia_session_get_dtmf( pjmedia_session *session,
					      unsigned index,
					      char *ascii_digits,
					      unsigned *size )
{
    PJ_ASSERT_RETURN(session && ascii_digits && size, PJ_EINVAL);
    return pjmedia_stream_get_dtmf(session->stream[index], ascii_digits,
				   size);
}
