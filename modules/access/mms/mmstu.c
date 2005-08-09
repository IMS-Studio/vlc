/*****************************************************************************
 * mms.c: MMS access plug-in
 *****************************************************************************
 * Copyright (C) 2001, 2002 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/


/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <vlc/vlc.h>
#include <string.h>
#include <vlc/input.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif
#ifdef HAVE_SYS_TIME_H
#   include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H 
#   include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif

#include "network.h"
#include "asf.h"
#include "buffer.h"

#include "mms.h"
#include "mmstu.h"

#undef MMS_DEBUG

/****************************************************************************
 * NOTES:
 *  MMSProtocole documentation found at http://get.to/sdp
 ****************************************************************************/

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
int  E_( MMSTUOpen )  ( access_t * );
void E_( MMSTUClose ) ( access_t * );


static int Read( access_t *, uint8_t *, int );
static int Seek( access_t *, int64_t );
static int Control( access_t *, int, va_list );

static int  MMSOpen ( access_t *, vlc_url_t *, int );
static int  MMSStart( access_t *, uint32_t );
static int  MMSStop ( access_t * );
static void MMSClose( access_t * );


static int  mms_CommandRead( access_t *p_access, int i_command1, int i_command2 );
static int  mms_CommandSend( access_t *, int, uint32_t, uint32_t, uint8_t *, int );

static int  mms_HeaderMediaRead( access_t *, int );

static int  mms_ReceivePacket( access_t * );


/*
 * XXX DON'T FREE MY MEMORY !!! XXX
 * non mais :P
 */
/*
 * Ok, ok, j'le ferai plus...
 */
/*
 * Merci :))
 */
/*
 * Vous pourriez signer vos commentaires (m�e si on voit bien qui peut
 * �rire ce genre de trucs :p), et �rire en anglais, bordel de
 * merde :p.
 */
/*
 * Alors la ouai �est fou les gens qui �rivent des commentaires sans les
 * signer. Ca m�iterait un coup de pied dans le cul � :)
 */

int  E_(MMSTUOpen)( access_t *p_access )
{
    access_sys_t   *p_sys;
    int             i_proto;
    int             i_status;

    /* Set up p_access */
    p_access->pf_read = Read;
    p_access->pf_block = NULL;
    p_access->pf_control = Control;
    p_access->pf_seek = Seek;
    p_access->info.i_update = 0;
    p_access->info.i_size = 0;
    p_access->info.i_pos = 0;
    p_access->info.b_eof = VLC_FALSE;
    p_access->info.i_title = 0;
    p_access->info.i_seekpoint = 0;
    p_access->p_sys = p_sys = malloc( sizeof( access_sys_t ) );
    memset( p_sys, 0, sizeof( access_sys_t ) );

    /* *** Parse URL and get server addr/port and path *** */
    vlc_UrlParse( &p_sys->url, p_access->psz_path, 0 );
    if( p_sys->url.psz_host == NULL || *p_sys->url.psz_host == '\0' )
    {
        msg_Err( p_access, "invalid server name" );
        vlc_UrlClean( &p_sys->url );
        return VLC_EGENERIC;
    }
    if( p_sys->url.i_port <= 0 )
    {
        p_sys->url.i_port = 1755;
    }

    /* *** connect to this server *** */
    /* look at  requested protocol (udp/tcp) */
    i_proto = MMS_PROTO_AUTO;
    if( *p_access->psz_access )
    {
        if( !strncmp( p_access->psz_access, "mmsu", 4 ) )
        {
            i_proto = MMS_PROTO_UDP;
        }
        else if( !strncmp( p_access->psz_access, "mmst", 4 ) )
        {
            i_proto = MMS_PROTO_TCP;
        }
    }

    /* connect */
    if( i_proto == MMS_PROTO_AUTO )
    {   /* first try with TCP and then UDP*/
        if( ( i_status = MMSOpen( p_access, &p_sys->url, MMS_PROTO_TCP ) ) )
        {
            i_status = MMSOpen( p_access, &p_sys->url, MMS_PROTO_UDP );
        }
    }
    else
    {
        i_status = MMSOpen( p_access, &p_sys->url, i_proto );
    }

    if( i_status )
    {
        msg_Err( p_access, "cannot connect to server" );
        vlc_UrlClean( &p_sys->url );
        return VLC_EGENERIC;
    }

    msg_Dbg( p_access, "connected to %s:%d", p_sys->url.psz_host, p_sys->url.i_port );
    /*
     * i_flags_broadcast
     *  yy xx ?? ??
     *  broadcast    yy=0x02, xx= 0x00
     *  pre-recorded yy=0x01, xx= 0x80 if video, 0x00 no video
     */
    if( p_sys->i_packet_count <= 0 && p_sys->asfh.i_data_packets_count > 0 )
    {
        p_sys->i_packet_count = p_sys->asfh.i_data_packets_count;
    }
    if( p_sys->i_packet_count <= 0 || ( p_sys->i_flags_broadcast >> 24 ) == 0x02 )
    {
        p_sys->b_seekable = VLC_FALSE;
    }
    else
    {
        p_sys->b_seekable = VLC_TRUE;
        p_access->info.i_size =
            (uint64_t)p_sys->i_header +
            (uint64_t)p_sys->i_packet_count * (uint64_t)p_sys->i_packet_length;
    }

    /* *** Start stream *** */
    if( MMSStart( p_access, 0xffffffff ) < 0 )
    {
        msg_Err( p_access, "cannot start stream" );
        MMSClose( p_access );
        vlc_UrlClean( &p_sys->url );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: free unused data structures
 *****************************************************************************/
void E_(MMSTUClose)( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    /* close connection with server */
    MMSClose( p_access );

    /* free memory */
    vlc_UrlClean( &p_sys->url );

    free( p_sys );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    access_sys_t *p_sys = p_access->p_sys;
    vlc_bool_t   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;
    int           i_int;
    vlc_value_t  val;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = p_sys->b_seekable;
            break;

        case ACCESS_CAN_FASTSEEK:
        case ACCESS_CAN_PAUSE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_FALSE;
            break;

        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );

#if 0       /* Disable for now until we have a clock synchro algo
             * which works with something else than MPEG over UDP */
            *pb_bool = VLC_FALSE;
#endif
            *pb_bool = VLC_TRUE;
            break;

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 3 * p_sys->i_packet_length;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            var_Get( p_access, "mms-caching", &val );
            *pi_64 = (int64_t)var_GetInteger( p_access, "mms-caching" ) * I64C(1000);
            break;

        case ACCESS_GET_PRIVATE_ID_STATE:
            i_int = (int)va_arg( args, int );
            pb_bool = (vlc_bool_t *)va_arg( args, vlc_bool_t * );

            if( i_int < 0 || i_int > 127 )
                return VLC_EGENERIC;
            *pb_bool =  p_sys->asfh.stream[i_int].i_selected ? VLC_TRUE : VLC_FALSE;
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
        case ACCESS_GET_TITLE_INFO:
        case ACCESS_SET_TITLE:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
            return VLC_EGENERIC;


        default:
            msg_Warn( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Seek: try to go at the right place
 *****************************************************************************/
static int Seek( access_t * p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;
    uint32_t    i_packet;
    uint32_t    i_offset;
    var_buffer_t buffer;

    if( i_pos < 0 )
        return VLC_EGENERIC;

    if( i_pos < p_sys->i_header)
    {

        if( p_access->info.i_pos < p_sys->i_header )
        {
            /* no need to restart stream, it was already one
             * or no stream was yet read */
            p_access->info.i_pos = i_pos;
            return VLC_SUCCESS;
        }
        else
        {
            i_packet = 0xffffffff;
            i_offset = 0;
        }
    }
    else
    {
        i_packet = ( i_pos - p_sys->i_header ) / p_sys->i_packet_length;
        i_offset = ( i_pos - p_sys->i_header ) % p_sys->i_packet_length;
    }
    msg_Dbg( p_access, "seeking to "I64Fd " (packet:%d)", i_pos, i_packet );

    MMSStop( p_access );
    msg_Dbg( p_access, "stream stopped (seek)" );

    /* *** restart stream *** */
    var_buffer_initwrite( &buffer, 0 );
    var_buffer_add64( &buffer, 0 ); /* seek point in second */
    var_buffer_add32( &buffer, 0xffffffff );
    var_buffer_add32( &buffer, i_packet ); // begin from start
    var_buffer_add8( &buffer, 0xff ); // stream time limit
    var_buffer_add8( &buffer, 0xff ); //  on 3bytes ...
    var_buffer_add8( &buffer, 0xff ); //
    var_buffer_add8( &buffer, 0x00 ); // don't use limit
    var_buffer_add32( &buffer, p_sys->i_media_packet_id_type );

    mms_CommandSend( p_access, 0x07, p_sys->i_command_level, 0x0001ffff,
                     buffer.p_data, buffer.i_data );

    var_buffer_free( &buffer );


    while( !p_access->b_die )
    {
        mms_HeaderMediaRead( p_access, MMS_PACKET_CMD );
        if( p_sys->i_command == 0x1e )
        {
            msg_Dbg( p_access, "received 0x1e (seek)" );
            break;
        }
    }

    while( !p_access->b_die )
    {
        mms_HeaderMediaRead( p_access, MMS_PACKET_CMD );
        if( p_sys->i_command == 0x05 )
        {
            msg_Dbg( p_access, "received 0x05 (seek)" );
            break;
        }
    }

    /* get a packet */
    mms_HeaderMediaRead( p_access, MMS_PACKET_MEDIA );
    msg_Dbg( p_access, "Streaming restarted" );

    p_sys->i_media_used += i_offset;
    p_access->info.i_pos = i_pos;
    p_access->info.b_eof = VLC_FALSE;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Read:
 *****************************************************************************/
static int Read( access_t *p_access, uint8_t *p_buffer, int i_len )
{
    access_sys_t *p_sys = p_access->p_sys;
    size_t      i_data;
    size_t      i_copy;

    i_data = 0;

    /* *** now send data if needed *** */
    while( i_data < (size_t)i_len )
    {
        if( p_access->info.i_pos < p_sys->i_header )
        {
            i_copy = __MIN( i_len, p_sys->i_header - p_access->info.i_pos );
            memcpy( &p_buffer[i_data], &p_sys->p_header[p_access->info.i_pos], i_copy );
            i_data += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else if( p_sys->i_media_used < p_sys->i_media )
        {
            i_copy = __MIN( i_len - i_data ,
                            p_sys->i_media - p_sys->i_media_used );
            memcpy( &p_buffer[i_data], &p_sys->p_media[p_sys->i_media_used], i_copy );
            i_data += i_copy;
            p_sys->i_media_used += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else if( p_sys->p_media != NULL &&
                 p_sys->i_media_used < p_sys->i_packet_length )
        {
            i_copy = __MIN( i_len - i_data,
                            p_sys->i_packet_length - p_sys->i_media_used);
            memset( &p_buffer[i_data], 0, i_copy );

            i_data += i_copy;
            p_sys->i_media_used += i_copy;
            p_access->info.i_pos += i_copy;
        }
        else if( p_access->info.b_eof ||
                 mms_HeaderMediaRead( p_access, MMS_PACKET_MEDIA ) < 0 )
        {
            break;
        }
    }

    return i_data;
}

/****************************************************************************
 * MMSOpen : Open a connection with the server over mmst or mmsu
 ****************************************************************************/
static int MMSOpen( access_t  *p_access, vlc_url_t *p_url, int  i_proto )
{
    access_sys_t *p_sys = p_access->p_sys;
    int           b_udp = ( i_proto == MMS_PROTO_UDP ) ? 1 : 0;

    var_buffer_t buffer;
    char         tmp[4096];
    uint16_t     *p;
    int          i_server_version;
    int          i_tool_version;
    int          i_update_player_url;
    int          i_encryption_type;
    int          i;
    int          i_streams;
    int          i_first;


    /* *** Open a TCP connection with server *** */
    msg_Dbg( p_access, "waiting for connection..." );
    p_sys->i_handle_tcp = net_OpenTCP( p_access, p_url->psz_host, p_url->i_port );
    if( p_sys->i_handle_tcp < 0 )
    {
        msg_Err( p_access, "failed to open a connection (tcp)" );
        return VLC_EGENERIC;
    }
    msg_Dbg( p_access,
             "connection(tcp) with \"%s:%d\" successful",
             p_url->psz_host,
             p_url->i_port );

    /* *** Bind port if UDP protocol is selected *** */
    if( b_udp )
    {
        if( net_GetSockAddress( p_sys->i_handle_tcp, p_sys->sz_bind_addr,
                                NULL ) )
        {
            net_Close( p_sys->i_handle_tcp );
            return VLC_EGENERIC;
        }

        p_sys->i_handle_udp = net_OpenUDP( p_access, p_sys->sz_bind_addr,
                                           7000, "", 0 );
        if( p_sys->i_handle_udp < 0 )
        {
            msg_Err( p_access, "failed to open a connection (udp)" );
            net_Close( p_sys->i_handle_tcp );
            return VLC_EGENERIC;
        }
        msg_Dbg( p_access,
                 "connection(udp) at \"%s:%d\" successful",
                 p_sys->sz_bind_addr, 7000 );
    }

    /* *** Init context for mms prototcol *** */
    E_( GenerateGuid )( &p_sys->guid );    /* used to identify client by server */
    msg_Dbg( p_access,
             "generated guid: "GUID_FMT,
             GUID_PRINT( p_sys->guid ) );
    p_sys->i_command_level = 1;          /* updated after 0x1A command */
    p_sys->i_seq_num = 0;
    p_sys->i_media_packet_id_type  = 0x04;
    p_sys->i_header_packet_id_type = 0x02;
    p_sys->i_proto = i_proto;
    p_sys->i_packet_seq_num = 0;
    p_sys->p_header = NULL;
    p_sys->i_header = 0;
    p_sys->p_media = NULL;
    p_sys->i_media = 0;
    p_sys->i_media_used = 0;

    p_access->info.i_pos = 0;
    p_sys->i_buffer_tcp = 0;
    p_sys->i_buffer_udp = 0;
    p_sys->p_cmd = NULL;
    p_sys->i_cmd = 0;
    p_access->info.b_eof = 0;

    /* *** send command 1 : connection request *** */
    var_buffer_initwrite( &buffer, 0 );
    var_buffer_add16( &buffer, 0x001c );
    var_buffer_add16( &buffer, 0x0003 );
    sprintf( tmp,
             "NSPlayer/7.0.0.1956; {"GUID_FMT"}; Host: %s",
             GUID_PRINT( p_sys->guid ),
             p_url->psz_host );
    var_buffer_addUTF16( &buffer, tmp );

    mms_CommandSend( p_access,
                     0x01,          /* connexion request */
                     0x00000000,    /* flags, FIXME */
                     0x0004000b,    /* ???? */
                     buffer.p_data,
                     buffer.i_data );

    if( mms_CommandRead( p_access, 0x01, 0 ) < 0 )
    {
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }

    i_server_version = GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 32 );
    i_tool_version = GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 36 );
    i_update_player_url = GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 40 );
    i_encryption_type = GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 44 );
    p = (uint16_t*)( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 48 );
#define GETUTF16( psz, size ) \
    { \
        int i; \
        psz = malloc( size + 1); \
        for( i = 0; i < size; i++ ) \
        { \
            psz[i] = p[i]; \
        } \
        psz[size] = '\0'; \
        p += ( size ); \
    }
    GETUTF16( p_sys->psz_server_version, i_server_version );
    GETUTF16( p_sys->psz_tool_version, i_tool_version );
    GETUTF16( p_sys->psz_update_player_url, i_update_player_url );
    GETUTF16( p_sys->psz_encryption_type, i_encryption_type );
#undef GETUTF16
    msg_Dbg( p_access,
             "0x01 --> server_version:\"%s\" tool_version:\"%s\" update_player_url:\"%s\" encryption_type:\"%s\"",
             p_sys->psz_server_version,
             p_sys->psz_tool_version,
             p_sys->psz_update_player_url,
             p_sys->psz_encryption_type );

    /* *** should make an 18 command to make data timing *** */

    /* *** send command 2 : transport protocol selection *** */
    var_buffer_reinitwrite( &buffer, 0 );
    var_buffer_add32( &buffer, 0x00000000 );
    var_buffer_add32( &buffer, 0x000a0000 );
    var_buffer_add32( &buffer, 0x00000002 );
    if( b_udp )
    {
        sprintf( tmp,
                 "\\\\%s\\UDP\\%d",
                 p_sys->sz_bind_addr,
                 7000 ); // FIXME
    }
    else
    {
        sprintf( tmp, "\\\\192.168.0.1\\TCP\\1242"  );
    }
    var_buffer_addUTF16( &buffer, tmp );
    var_buffer_add16( &buffer, '0' );

    mms_CommandSend( p_access,
                     0x02,          /* connexion request */
                     0x00000000,    /* flags, FIXME */
                     0xffffffff,    /* ???? */
                     buffer.p_data,
                     buffer.i_data );

    /* *** response from server, should be 0x02 or 0x03 *** */
    mms_CommandRead( p_access, 0x02, 0x03 );
    if( p_sys->i_command == 0x03 )
    {
        msg_Err( p_access,
                 "%s protocol selection failed", b_udp ? "UDP" : "TCP" );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }
    else if( p_sys->i_command != 0x02 )
    {
        msg_Warn( p_access, "received command isn't 0x02 in reponse to 0x02" );
    }

    /* *** send command 5 : media file name/path requested *** */
    var_buffer_reinitwrite( &buffer, 0 );
    var_buffer_add64( &buffer, 0 );
    var_buffer_addUTF16( &buffer, p_url->psz_path );

    mms_CommandSend( p_access,
                     0x05,
                     p_sys->i_command_level,
                     0xffffffff,
                     buffer.p_data,
                     buffer.i_data );

    /* *** wait for reponse *** */
    mms_CommandRead( p_access, 0x1a, 0x06 );

    /* test if server send 0x1A answer */
    if( p_sys->i_command == 0x1A )
    {
        msg_Err( p_access, "id/password requested (not yet supported)" );
        /*  FIXME */
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }
    if( p_sys->i_command != 0x06 )
    {
        msg_Err( p_access,
                 "unknown answer (0x%x instead of 0x06)",
                 p_sys->i_command );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return( -1 );
    }

    /*  1 for file ok, 2 for authen ok */
    switch( GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE ) )
    {
        case 0x0001:
            msg_Dbg( p_access, "Media file name/path accepted" );
            break;
        case 0x0002:
            msg_Dbg( p_access, "Authentication accepted" );
            break;
        case -1:
        default:
        msg_Err( p_access, "error while asking for file %d",
                 GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE ) );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }

    p_sys->i_flags_broadcast =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 12 );
    p_sys->i_media_length =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 24 );
    p_sys->i_packet_length =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 44 );
    p_sys->i_packet_count =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 48 );
    p_sys->i_max_bit_rate =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 56 );
    p_sys->i_header_size =
        GetDWLE( p_sys->p_cmd + MMS_CMD_HEADERSIZE + 60 );

    msg_Dbg( p_access,
             "answer 0x06 flags:0x%8.8x media_length:%ds packet_length:%d packet_count:%d max_bit_rate:%d header_size:%d",
             p_sys->i_flags_broadcast,
             p_sys->i_media_length,
             p_sys->i_packet_length,
             p_sys->i_packet_count,
             p_sys->i_max_bit_rate,
             p_sys->i_header_size );

    /* *** send command 15 *** */

    var_buffer_reinitwrite( &buffer, 0 );
    var_buffer_add32( &buffer, 0 );
    var_buffer_add32( &buffer, 0x8000 );
    var_buffer_add32( &buffer, 0xffffffff );
    var_buffer_add32( &buffer, 0x00 );
    var_buffer_add32( &buffer, 0x00 );
    var_buffer_add32( &buffer, 0x00 );
    var_buffer_add64( &buffer, (((uint64_t)0x40ac2000)<<32) );
    var_buffer_add32( &buffer, p_sys->i_header_packet_id_type );
    var_buffer_add32( &buffer, 0x00 );
    mms_CommandSend( p_access, 0x15, p_sys->i_command_level, 0x00,
                     buffer.p_data, buffer.i_data );

    /* *** wait for reponse *** */
    /* Commented out because it fails on some stream (no 0x11 answer) */
#if 0
    mms_CommandRead( p_access, 0x11, 0 );

    if( p_sys->i_command != 0x11 )
    {
        msg_Err( p_access,
                 "unknown answer (0x%x instead of 0x11)",
                 p_sys->i_command );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return( -1 );
    }
#endif

    /* *** now read header packet *** */
    /* XXX could be split over multiples packets */
    msg_Dbg( p_access, "reading header" );
    for( ;; )
    {
        if( mms_HeaderMediaRead( p_access, MMS_PACKET_HEADER ) < 0 )
        {
            msg_Err( p_access, "cannot receive header" );
            var_buffer_free( &buffer );
            MMSClose( p_access );
            return VLC_EGENERIC;
        }
        if( p_sys->i_header >= p_sys->i_header_size )
        {
            msg_Dbg( p_access,
                     "header complete(%d)",
                     p_sys->i_header );
            break;
        }
        msg_Dbg( p_access,
                 "header incomplete (%d/%d), reading more",
                 p_sys->i_header,
                 p_sys->i_header_size );
    }

    /* *** parse header and get stream and their id *** */
    /* get all streams properties,
     *
     * TODO : stream bitrates properties(optional)
     *        and bitrate mutual exclusion(optional) */
    E_( asf_HeaderParse )( &p_sys->asfh,
                           p_sys->p_header, p_sys->i_header );
    E_( asf_StreamSelect)( &p_sys->asfh,
                           var_CreateGetInteger( p_access, "mms-maxbitrate" ),
                           var_CreateGetInteger( p_access, "mms-all" ),
                           var_CreateGetInteger( p_access, "audio" ),
                           var_CreateGetInteger( p_access, "video" ) );

    /* *** now select stream we want to receive *** */
    /* TODO take care of stream bitrate TODO */
    i_streams = 0;
    i_first = -1;
    var_buffer_reinitwrite( &buffer, 0 );
    /* for now, select first audio and video stream */
    for( i = 1; i < 128; i++ )
    {

        if( p_sys->asfh.stream[i].i_cat != ASF_STREAM_UNKNOWN )
        {
            i_streams++;
            if( i_first != -1 )
            {
                var_buffer_add16( &buffer, 0xffff );
                var_buffer_add16( &buffer, i );
            }
            else
            {
                i_first = i;
            }
            if( p_sys->asfh.stream[i].i_selected )
            {
                var_buffer_add16( &buffer, 0x0000 );
                msg_Info( p_access,
                          "selecting stream[0x%x] %s (%d kb/s)",
                          i,
                          ( p_sys->asfh.stream[i].i_cat == ASF_STREAM_AUDIO  ) ?
                                                  "audio" : "video" ,
                          p_sys->asfh.stream[i].i_bitrate / 1024);
            }
            else
            {
                var_buffer_add16( &buffer, 0x0002 );
                msg_Info( p_access,
                          "ignoring stream[0x%x] %s (%d kb/s)",
                          i,
                          ( p_sys->asfh.stream[i].i_cat == ASF_STREAM_AUDIO  ) ?
                                    "audio" : "video" ,
                          p_sys->asfh.stream[i].i_bitrate / 1024);

            }
        }
    }

    if( i_streams == 0 )
    {
        msg_Err( p_access, "cannot find any stream" );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }
    mms_CommandSend( p_access, 0x33,
                     i_streams,
                     0xffff | ( i_first << 16 ),
                     buffer.p_data, buffer.i_data );

    mms_CommandRead( p_access, 0x21, 0 );
    if( p_sys->i_command != 0x21 )
    {
        msg_Err( p_access,
                 "unknown answer (0x%x instead of 0x21)",
                 p_sys->i_command );
        var_buffer_free( &buffer );
        MMSClose( p_access );
        return VLC_EGENERIC;
    }


    var_buffer_free( &buffer );

    msg_Info( p_access, "connection successful" );

    return VLC_SUCCESS;
}

/****************************************************************************
 * MMSStart : Start streaming
 ****************************************************************************/
static int MMSStart( access_t  *p_access, uint32_t i_packet )
{
    access_sys_t        *p_sys = p_access->p_sys;
    var_buffer_t    buffer;

    /* *** start stream from packet 0 *** */
    var_buffer_initwrite( &buffer, 0 );
    var_buffer_add64( &buffer, 0 ); /* seek point in second */
    var_buffer_add32( &buffer, 0xffffffff );
    var_buffer_add32( &buffer, i_packet ); // begin from start
    var_buffer_add8( &buffer, 0xff ); // stream time limit
    var_buffer_add8( &buffer, 0xff ); //  on 3bytes ...
    var_buffer_add8( &buffer, 0xff ); //
    var_buffer_add8( &buffer, 0x00 ); // don't use limit
    var_buffer_add32( &buffer, p_sys->i_media_packet_id_type );

    mms_CommandSend( p_access, 0x07, p_sys->i_command_level, 0x0001ffff,
                     buffer.p_data, buffer.i_data );

    var_buffer_free( &buffer );

    mms_CommandRead( p_access, 0x05, 0 );

    if( p_sys->i_command != 0x05 )
    {
        msg_Err( p_access,
                 "unknown answer (0x%x instead of 0x05)",
                 p_sys->i_command );
        return( -1 );
    }
    else
    {
        /* get a packet */
        mms_HeaderMediaRead( p_access, MMS_PACKET_MEDIA );
        msg_Dbg( p_access, "Streaming started" );
        return( 0 );
    }
}

/****************************************************************************
 * MMSStop : Stop streaming
 ****************************************************************************/
static int MMSStop( access_t  *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    /* *** stop stream but keep connection alive *** */
    mms_CommandSend( p_access,
                     0x09,
                     p_sys->i_command_level,
                     0x001fffff,
                     NULL, 0 );
    return( 0 );
}

/****************************************************************************
 * MMSClose : Close streaming and connection
 ****************************************************************************/
static void MMSClose( access_t  *p_access )
{
    access_sys_t        *p_sys = p_access->p_sys;

    msg_Dbg( p_access, "Connection closed" );

    /* *** tell server that we will disconnect *** */
    mms_CommandSend( p_access,
                     0x0d,
                     p_sys->i_command_level,
                     0x00000001,
                     NULL, 0 );

    /* *** close sockets *** */
    net_Close( p_sys->i_handle_tcp );
    if( p_sys->i_proto == MMS_PROTO_UDP )
    {
        net_Close( p_sys->i_handle_udp );
    }

    FREE( p_sys->p_cmd );
    FREE( p_sys->p_media );
    FREE( p_sys->p_header );

    FREE( p_sys->psz_server_version );
    FREE( p_sys->psz_tool_version );
    FREE( p_sys->psz_update_player_url );
    FREE( p_sys->psz_encryption_type );
}

/****************************************************************************
 *
 * MMS specific functions
 *
 ****************************************************************************/
static int mms_CommandSend( access_t *p_access,
                             int i_command,
                             uint32_t i_prefix1, uint32_t i_prefix2,
                             uint8_t *p_data, int i_data_old )
{
    var_buffer_t buffer;

    access_sys_t        *p_sys = p_access->p_sys;
    int i_data_by8;
    int i_data = i_data_old;

    while( i_data & 0x7 ) i_data++;
    i_data_by8 = i_data >> 3;

    /* first init buffer */
    var_buffer_initwrite( &buffer, 0 );

    var_buffer_add32( &buffer, 0x00000001 );    /* start sequence */
    var_buffer_add32( &buffer, 0xB00BFACE );
    /* size after protocol type */
    var_buffer_add32( &buffer, i_data + MMS_CMD_HEADERSIZE - 16 );
    var_buffer_add32( &buffer, 0x20534d4d );    /* protocol "MMS " */
    var_buffer_add32( &buffer, i_data_by8 + 4 );
    var_buffer_add32( &buffer, p_sys->i_seq_num ); p_sys->i_seq_num++;
    var_buffer_add64( &buffer, 0 );
    var_buffer_add32( &buffer, i_data_by8 + 2 );
    var_buffer_add32( &buffer, 0x00030000 | i_command ); /* dir | command */
    var_buffer_add32( &buffer, i_prefix1 );    /* command specific */
    var_buffer_add32( &buffer, i_prefix2 );    /* command specific */

    /* specific command data */
    if( p_data && i_data > 0 )
    {
        var_buffer_addmemory( &buffer, p_data, i_data_old );
    }

    /* Append padding to the command data */
    var_buffer_add64( &buffer, 0 );

    /* send it */
    if( send( p_sys->i_handle_tcp,
              buffer.p_data,
              buffer.i_data - ( 8 - ( i_data - i_data_old ) ),
              0 ) == -1 )
    {
        msg_Err( p_access, "failed to send command" );
        return VLC_EGENERIC;
    }

    var_buffer_free( &buffer );
    return VLC_SUCCESS;
}

static int NetFillBuffer( access_t *p_access )
{
#ifdef UNDER_CE
    return -1;

#else
    access_sys_t    *p_sys = p_access->p_sys;
    struct timeval  timeout;
    fd_set          fds_r, fds_e;
    int             i_ret;

    /* FIXME when using udp */
    ssize_t i_tcp, i_udp;
    ssize_t i_tcp_read, i_udp_read;
    int i_handle_max;
    int i_try = 0;

    i_tcp = MMS_BUFFER_SIZE/2 - p_sys->i_buffer_tcp;

    if( p_sys->i_proto == MMS_PROTO_UDP )
    {
        i_udp = MMS_BUFFER_SIZE/2 - p_sys->i_buffer_udp;
    }
    else
    {
        i_udp = 0;  /* there isn't udp socket */
    }

    i_handle_max = 0;

    if( i_tcp > 0 )
        i_handle_max = __MAX( i_handle_max, p_sys->i_handle_tcp );
    if( i_udp > 0 )
        i_handle_max = __MAX( i_handle_max, p_sys->i_handle_udp );

    if( i_handle_max == 0 )
    {
        msg_Warn( p_access, "nothing to read %d:%d", i_tcp, i_udp );
        return 0;
    }
    else
    {
        /* msg_Warn( p_access, "ask for tcp:%d udp:%d", i_tcp, i_udp ); */
    }

    /* Find if some data is available */
    do
    {
        i_try++;

        /* Initialize file descriptor set */
        FD_ZERO( &fds_r );
        FD_ZERO( &fds_e );

        if( i_tcp > 0 )
        {
            FD_SET( p_sys->i_handle_tcp, &fds_r );
            FD_SET( p_sys->i_handle_tcp, &fds_e );
        }
        if( i_udp > 0 )
        {
            FD_SET( p_sys->i_handle_udp, &fds_r );
            FD_SET( p_sys->i_handle_udp, &fds_e );
        }

        /* We'll wait 0.5 second if nothing happens */
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;

        if( i_try > 3 && (p_sys->i_buffer_tcp > 0 || p_sys->i_buffer_udp > 0) )
        {
            return -1;
        }

        if( p_access->b_die || p_access->b_error ) return -1;

        //msg_Dbg( p_access, "NetFillBuffer: trying again (select)" );

    } while( !(i_ret = select(i_handle_max +1, &fds_r, 0, &fds_e, &timeout)) ||
             (i_ret < 0 && errno == EINTR) );

    if( i_ret < 0 )
    {
        msg_Err( p_access, "network select error (%s)", strerror(errno) );
        return -1;
    }

    i_tcp_read = i_udp_read = 0;

    if( i_tcp > 0 && FD_ISSET( p_sys->i_handle_tcp, &fds_r ) )
    {
        i_tcp_read =
            recv( p_sys->i_handle_tcp,
                  p_sys->buffer_tcp + p_sys->i_buffer_tcp,
                  i_tcp + MMS_BUFFER_SIZE/2, 0 );
    }

    if( i_udp > 0 && FD_ISSET( p_sys->i_handle_udp, &fds_r ) )
    {
        i_udp_read = recv( p_sys->i_handle_udp,
                           p_sys->buffer_udp + p_sys->i_buffer_udp,
                           i_udp + MMS_BUFFER_SIZE/2, 0 );
    }

#if MMS_DEBUG
    if( p_sys->i_proto == MMS_PROTO_UDP )
    {
        msg_Dbg( p_access, "filling buffer TCP:%d+%d UDP:%d+%d",
                 p_sys->i_buffer_tcp, i_tcp_read,
                 p_sys->i_buffer_udp, i_udp_read );
    }
    else
    {
        msg_Dbg( p_access, "filling buffer TCP:%d+%d",
                 p_sys->i_buffer_tcp, i_tcp_read );
    }
#endif

    if( i_tcp_read > 0 ) p_sys->i_buffer_tcp += i_tcp_read;
    if( i_udp_read > 0 ) p_sys->i_buffer_udp += i_udp_read;

    return i_tcp_read + i_udp_read;
#endif
}

static int  mms_ParseCommand( access_t *p_access,
                              uint8_t *p_data,
                              int i_data,
                              int *pi_used )
{
 #define GET32( i_pos ) \
    ( p_sys->p_cmd[i_pos] + ( p_sys->p_cmd[i_pos +1] << 8 ) + \
      ( p_sys->p_cmd[i_pos + 2] << 16 ) + \
      ( p_sys->p_cmd[i_pos + 3] << 24 ) )

    access_sys_t        *p_sys = p_access->p_sys;
    int         i_length;
    uint32_t    i_id;

    if( p_sys->p_cmd )
    {
        free( p_sys->p_cmd );
    }
    p_sys->i_cmd = i_data;
    p_sys->p_cmd = malloc( i_data );
    memcpy( p_sys->p_cmd, p_data, i_data );

    *pi_used = i_data; /* by default */

    if( i_data < MMS_CMD_HEADERSIZE )
    {
        msg_Warn( p_access, "truncated command (header incomplete)" );
        p_sys->i_command = 0;
        return -1;
    }
    i_id =  GetDWLE( p_data + 4 );
    i_length = GetDWLE( p_data + 8 ) + 16;

    if( i_id != 0xb00bface )
    {
        msg_Err( p_access,
                 "incorrect command header (0x%x)", i_id );
        p_sys->i_command = 0;
        return -1;
    }

    if( i_length > p_sys->i_cmd )
    {
        msg_Warn( p_access,
                  "truncated command (missing %d bytes)",
                   i_length - i_data  );
        p_sys->i_command = 0;
        return -1;
    }
    else if( i_length < p_sys->i_cmd )
    {
        p_sys->i_cmd = i_length;
        *pi_used = i_length;
    }

    msg_Dbg( p_access,
             "recv command start_sequence:0x%8.8x command_id:0x%8.8x length:%d len8:%d sequence 0x%8.8x len8_II:%d dir_comm:0x%8.8x",
             GET32( 0 ),
             GET32( 4 ),
             GET32( 8 ),
             /* 12: protocol type "MMS " */
             GET32( 16 ),
             GET32( 20 ),
             /* 24: unknown (0) */
             /* 28: unknown (0) */
             GET32( 32 ),
             GET32( 36 )
             /* 40: switches */
             /* 44: extra */ );

    p_sys->i_command = GET32( 36 ) & 0xffff;
#undef GET32

    return MMS_PACKET_CMD;
}

static int  mms_ParsePacket( access_t *p_access,
                             uint8_t *p_data, size_t i_data,
                             int *pi_used )
{
    access_sys_t        *p_sys = p_access->p_sys;
    int i_packet_seq_num;
    size_t i_packet_length;
    uint32_t i_packet_id;

    uint8_t  *p_packet;


    *pi_used = i_data; /* default */
    if( i_data <= 8 )
    {
        msg_Warn( p_access, "truncated packet (header incomplete)" );
        return -1;
    }

    i_packet_id = p_data[4];
    i_packet_seq_num = GetDWLE( p_data );
    i_packet_length = GetWLE( p_data + 6 );

    //msg_Warn( p_access, "------->i_packet_length=%d, i_data=%d", i_packet_length, i_data );

    if( i_packet_length > i_data || i_packet_length <= 8)
    {
     /*   msg_Dbg( p_access,
                 "truncated packet (Declared %d bytes, Actual %d bytes)",
                 i_packet_length, i_data  ); */
        *pi_used = 0;
        return -1;
    }
    else if( i_packet_length < i_data )
    {
        *pi_used = i_packet_length;
    }

    if( i_packet_id == 0xff )
    {
        msg_Warn( p_access,
                  "receive MMS UDP pair timing" );
        return( MMS_PACKET_UDP_TIMING );
    }

    if( i_packet_id != p_sys->i_header_packet_id_type &&
        i_packet_id != p_sys->i_media_packet_id_type )
    {
        msg_Warn( p_access, "incorrect Packet Id Type (0x%x)", i_packet_id );
        return -1;
    }

    /* we now have a media or a header packet */
    p_packet = malloc( i_packet_length - 8 ); // don't bother with preheader
    memcpy( p_packet, p_data + 8, i_packet_length - 8 );

    if( i_packet_seq_num != p_sys->i_packet_seq_num )
    {
        /* FIXME for udp could be just wrong order ? */
        msg_Warn( p_access,
                  "detected packet lost (%d != %d)",
                  i_packet_seq_num,
                  p_sys->i_packet_seq_num );
        p_sys->i_packet_seq_num = i_packet_seq_num;
    }
    p_sys->i_packet_seq_num++;

    if( i_packet_id == p_sys->i_header_packet_id_type )
    {
        if( p_sys->p_header )
        {
            p_sys->p_header = realloc( p_sys->p_header,
                                          p_sys->i_header + i_packet_length - 8 );
            memcpy( &p_sys->p_header[p_sys->i_header],
                    p_packet,
                    i_packet_length - 8 );
            p_sys->i_header += i_packet_length - 8;

            free( p_packet );
        }
        else
        {
            p_sys->p_header = p_packet;
            p_sys->i_header = i_packet_length - 8;
        }
/*        msg_Dbg( p_access,
                 "receive header packet (%d bytes)",
                 i_packet_length - 8 ); */

        return MMS_PACKET_HEADER;
    }
    else
    {
        FREE( p_sys->p_media );
        p_sys->p_media = p_packet;
        p_sys->i_media = i_packet_length - 8;
        p_sys->i_media_used = 0;
/*        msg_Dbg( p_access,
                 "receive media packet (%d bytes)",
                 i_packet_length - 8 ); */

        return MMS_PACKET_MEDIA;
    }
}

static int mms_ReceivePacket( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i_packet_tcp_type;
    int i_packet_udp_type;

    for( ;; )
    {
        vlc_bool_t b_refill = VLC_TRUE;

        /* first if we need to refill buffer */
        if( p_sys->i_buffer_tcp >= MMS_CMD_HEADERSIZE )
        {
            if( GetDWLE( p_sys->buffer_tcp + 4 ) == 0xb00bface  )
            {
                if( GetDWLE( p_sys->buffer_tcp + 8 ) + 16 <=
                    (uint32_t)p_sys->i_buffer_tcp )
                {
                    b_refill = VLC_FALSE;
                }
            }
            else if( GetWLE( p_sys->buffer_tcp + 6 ) <= p_sys->i_buffer_tcp )
            {
                b_refill = VLC_FALSE;
            }
        }
        if( p_sys->i_proto == MMS_PROTO_UDP && p_sys->i_buffer_udp >= 8 &&
            GetWLE( p_sys->buffer_udp + 6 ) <= p_sys->i_buffer_udp )
        {
            b_refill = VLC_FALSE;
        }

        if( b_refill && NetFillBuffer( p_access ) < 0 )
        {
            msg_Warn( p_access, "cannot fill buffer" );
            return -1;
        }

        i_packet_tcp_type = -1;
        i_packet_udp_type = -1;

        if( p_sys->i_buffer_tcp > 0 )
        {
            int i_used;

            if( GetDWLE( p_sys->buffer_tcp + 4 ) == 0xb00bface )
            {
                i_packet_tcp_type =
                    mms_ParseCommand( p_access, p_sys->buffer_tcp,
                                      p_sys->i_buffer_tcp, &i_used );

            }
            else
            {
                i_packet_tcp_type =
                    mms_ParsePacket( p_access, p_sys->buffer_tcp,
                                     p_sys->i_buffer_tcp, &i_used );
            }
            if( i_used > 0 && i_used < MMS_BUFFER_SIZE )
            {
                memmove( p_sys->buffer_tcp, p_sys->buffer_tcp + i_used,
                         MMS_BUFFER_SIZE - i_used );
            }
            p_sys->i_buffer_tcp -= i_used;
        }
        else if( p_sys->i_buffer_udp > 0 )
        {
            int i_used;

            i_packet_udp_type =
                mms_ParsePacket( p_access, p_sys->buffer_udp,
                                 p_sys->i_buffer_udp, &i_used );

            if( i_used > 0 && i_used < MMS_BUFFER_SIZE )
            {
                memmove( p_sys->buffer_udp, p_sys->buffer_udp + i_used,
                         MMS_BUFFER_SIZE - i_used );
            }
            p_sys->i_buffer_udp -= i_used;
        }

        if( i_packet_tcp_type == MMS_PACKET_CMD && p_sys->i_command == 0x1b )
        {
            mms_CommandSend( p_access, 0x1b, 0, 0, NULL, 0 );
            i_packet_tcp_type = -1;
        }

        if( i_packet_tcp_type != -1 )
        {
            return i_packet_tcp_type;
        }
        else if( i_packet_udp_type != -1 )
        {
            return i_packet_udp_type;
        }
    }
}

static int mms_ReceiveCommand( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;

    for( ;; )
    {
        int i_used;
        int i_status;

        if( NetFillBuffer( p_access ) < 0 )
        {
            msg_Warn( p_access, "cannot fill buffer" );
            return VLC_EGENERIC;
        }
        if( p_sys->i_buffer_tcp > 0 )
        {
            i_status = mms_ParseCommand( p_access, p_sys->buffer_tcp,
                                         p_sys->i_buffer_tcp, &i_used );
            if( i_used < MMS_BUFFER_SIZE )
            {
                memmove( p_sys->buffer_tcp, p_sys->buffer_tcp + i_used,
                         MMS_BUFFER_SIZE - i_used );
            }
            p_sys->i_buffer_tcp -= i_used;

            if( i_status < 0 )
            {
                return VLC_EGENERIC;
            }

            if( p_sys->i_command == 0x1b )
            {
                mms_CommandSend( p_access, 0x1b, 0, 0, NULL, 0 );
            }
            else
            {
                break;
            }
        }
        else
        {
            return VLC_EGENERIC;
        }
    }

    return VLC_SUCCESS;
}

#define MMS_RETRY_MAX       10
#define MMS_RETRY_SLEEP     50000

static int mms_CommandRead( access_t *p_access, int i_command1,
                            int i_command2 )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i_count;
    int i_status;

    for( i_count = 0; i_count < MMS_RETRY_MAX; )
    {
        i_status = mms_ReceiveCommand( p_access );
        if( i_status < 0 || p_sys->i_command == 0 )
        {
            i_count++;
            msleep( MMS_RETRY_SLEEP );
        }
        else if( i_command1 == 0 && i_command2 == 0)
        {
            return VLC_SUCCESS;
        }
        else if( p_sys->i_command == i_command1 ||
                 p_sys->i_command == i_command2 )
        {
            return VLC_SUCCESS;
        }
        else
        {
            switch( p_sys->i_command )
            {
                case 0x03:
                    msg_Warn( p_access, "socket closed by server" );
                    p_access->info.b_eof = 1;
                    return VLC_EGENERIC;
                case 0x1e:
                    msg_Warn( p_access, "end of media stream" );
                    p_access->info.b_eof = 1;
                    return VLC_EGENERIC;
                default:
                    break;
            }
        }
    }
    msg_Warn( p_access, "failed to receive command (aborting)" );

    return VLC_EGENERIC;
}


static int mms_HeaderMediaRead( access_t *p_access, int i_type )
{
    access_sys_t *p_sys = p_access->p_sys;
    int          i_count;

    for( i_count = 0; i_count < MMS_RETRY_MAX; )
    {
        int i_status;

        if( p_access->b_die )
            return -1;

        i_status = mms_ReceivePacket( p_access );
        if( i_status < 0 )
        {
            i_count++;
            msg_Warn( p_access, "cannot receive header (%d/%d)",
                      i_count, MMS_RETRY_MAX );
            msleep( MMS_RETRY_SLEEP );
        }
        else if( i_status == i_type || i_type == MMS_PACKET_ANY )
        {
            return i_type;
        }
        else if( i_status == MMS_PACKET_CMD )
        {
            switch( p_sys->i_command )
            {
                case 0x03:
                    msg_Warn( p_access, "socket closed by server" );
                    p_access->info.b_eof = 1;
                    return -1;
                case 0x1e:
                    msg_Warn( p_access, "end of media stream" );
                    p_access->info.b_eof = 1;
                    return -1;
                case 0x20:
                    /* XXX not too dificult to be done EXCEPT that we
                     * need to restart demuxer... and I don't see how we
                     * could do that :p */
                    msg_Err( p_access,
                             "reinitialization needed --> unsupported" );
                    p_access->info.b_eof = VLC_TRUE;
                    return -1;
                default:
                    break;
            }
        }
    }

    msg_Err( p_access, "cannot receive %s (aborting)",
             ( i_type == MMS_PACKET_HEADER ) ? "header" : "media data" );
    return -1;
}

