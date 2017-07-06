/*******************************************************************************
 *
 *                              Delta Chat Core
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 *******************************************************************************
 *
 * File:    mrmailbox.c
 * Purpose: mrmailbox_t represents a single mailbox, see header for details.
 *
 ******************************************************************************/


#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h> /* for getpid() */
#include <unistd.h>    /* for getpid() */
#include <sqlite3.h>
#include <openssl/opensslv.h>
#include "mrmailbox.h"
#include "mrimap.h"
#include "mrsmtp.h"
#include "mrmimeparser.h"
#include "mrcontact.h"
#include "mrtools.h"
#include "mrjob.h"
#include "mrloginparam.h"
#include "mrkey.h"
#include "mrpgp.h"


/*******************************************************************************
 * Handle Groups
 ******************************************************************************/


static char* extract_grpid_from_messageid(const char* mid)
{
	/* extract our group ID from Message-IDs as `Gr.12345678.morerandom.user@domain.de`; "12345678" is the wanted ID in this example. */
	int success = 0;
	char* ret = NULL, *p1;

    if( mid == NULL || strlen(mid)<8 || mid[0]!='G' || mid[1]!='r' || mid[2]!='.' ) {
		goto cleanup;
    }

	ret = safe_strdup(&mid[3]);

	p1 = strchr(ret, '.');
	if( p1 == NULL ) {
		goto cleanup;
	}
	*p1 = 0;

	if( strlen(ret)!=MR_VALID_ID_LEN ) {
		goto cleanup;
	}

	success = 1;

cleanup:
	if( success == 0 ) { free(ret); ret = NULL; }
    return success? ret : NULL;
}


static char* get_first_grpid_from_char_clist(const clist* list)
{
	clistiter* cur;
	if( list ) {
		for( cur = clist_begin(list); cur!=NULL ; cur=clist_next(cur) ) {
			const char* mid = clist_content(cur);
			char* grpid = extract_grpid_from_messageid(mid);
			if( grpid ) {
				return grpid;
			}
		}
	}
	return NULL;
}


static uint32_t lookup_group_by_grpid__(mrmailbox_t* mailbox, mrmimeparser_t* mime_parser, int create_as_needed,
                                        uint32_t from_id, carray* to_list)
{
	/* search the grpid in the header */
	uint32_t              chat_id = 0;
	clistiter*            cur;
	struct mailimf_field* field;
	char*                 grpid1 = NULL, *grpid2 = NULL, *grpid3 = NULL, *grpid4 = NULL;
	const char*           grpid = NULL; /* must not be freed, just one of the others */
	char*                 grpname = NULL;
	sqlite3_stmt*         stmt;
	int                   i, to_list_cnt = carray_count(to_list);
	char*                 self_addr = NULL;
	int                   recreate_member_list = 0;

	/* special commands */
	char*                 X_MrRemoveFromGrp = NULL; /* pointer somewhere into mime_parser, must not be freed */
	char*                 X_MrAddToGrp = NULL; /* pointer somewhere into mime_parser, must not be freed */
	int                   X_MrGrpNameChanged = 0;

	for( cur = clist_begin(mime_parser->m_header->fld_list); cur!=NULL ; cur=clist_next(cur) )
	{
		field = (struct mailimf_field*)clist_content(cur);
		if( field )
		{
			if( field->fld_type == MAILIMF_FIELD_OPTIONAL_FIELD )
			{
				struct mailimf_optional_field* optional_field = field->fld_data.fld_optional_field;
				if( optional_field && optional_field->fld_name ) {
					if( strcasecmp(optional_field->fld_name, "X-MrGrpId")==0 || strcasecmp(optional_field->fld_name, "Chat-Group-ID")==0 ) {
						grpid1 = safe_strdup(optional_field->fld_value);
					}
					else if( strcasecmp(optional_field->fld_name, "X-MrGrpName")==0 || strcasecmp(optional_field->fld_name, "Chat-Group-Name")==0 ) {
						grpname = mr_decode_header_string(optional_field->fld_value);
					}
					else if( strcasecmp(optional_field->fld_name, "X-MrRemoveFromGrp")==0 || strcasecmp(optional_field->fld_name, "Chat-Group-Member-Removed")==0 ) {
						X_MrRemoveFromGrp = optional_field->fld_value;
					}
					else if( strcasecmp(optional_field->fld_name, "X-MrAddToGrp")==0 || strcasecmp(optional_field->fld_name, "Chat-Group-Member-Added")==0 ) {
						X_MrAddToGrp = optional_field->fld_value;
					}
					else if( strcasecmp(optional_field->fld_name, "X-MrGrpNameChanged")==0 || strcasecmp(optional_field->fld_name, "Chat-Group-Name-Changed")==0 ) {
						X_MrGrpNameChanged = 1;
					}
				}
			}
			else if( field->fld_type == MAILIMF_FIELD_MESSAGE_ID )
			{
				struct mailimf_message_id* fld_message_id = field->fld_data.fld_message_id;
				if( fld_message_id ) {
					grpid2 = extract_grpid_from_messageid(fld_message_id->mid_value);
				}
			}
			else if( field->fld_type == MAILIMF_FIELD_IN_REPLY_TO )
			{
				struct mailimf_in_reply_to* fld_in_reply_to = field->fld_data.fld_in_reply_to;
				if( fld_in_reply_to ) {
					grpid3 = get_first_grpid_from_char_clist(fld_in_reply_to->mid_list);
				}
			}
			else if( field->fld_type == MAILIMF_FIELD_REFERENCES )
			{
				struct mailimf_references* fld_references = field->fld_data.fld_references;
				if( fld_references ) {
					grpid4 = get_first_grpid_from_char_clist(fld_references->mid_list);
				}
			}

		}
	}

	grpid = grpid1? grpid1 : (grpid2? grpid2 : (grpid3? grpid3 : grpid4));
	if( grpid == NULL ) {
		goto cleanup;
	}

	/* check, if we have a chat with this group ID */
	stmt = mrsqlite3_predefine__(mailbox->m_sql, SELECT_id_FROM_CHATS_WHERE_grpid,
		"SELECT id FROM chats WHERE grpid=?;");
	sqlite3_bind_text (stmt, 1, grpid, -1, SQLITE_STATIC);
	if( sqlite3_step(stmt)==SQLITE_ROW ) {
		chat_id = sqlite3_column_int(stmt, 0);
	}

	/* check if the sender is a member of the existing group -
	if not, the message does not go to the group chat but to the normal chat with the sender */
	if( chat_id!=0 && !mrmailbox_is_contact_in_chat__(mailbox, chat_id, from_id) ) {
		chat_id = 0;
		goto cleanup;
	}

	/* check if the group does not exist but should be created */
	int group_explicitly_left = mrmailbox_group_explicitly_left__(mailbox, grpid);

	self_addr = mrsqlite3_get_config__(mailbox->m_sql, "configured_addr", "");
	if( chat_id == 0
	 && create_as_needed
	 && grpname
	 && X_MrRemoveFromGrp==NULL /*otherwise, a pending "quit" message may pop up*/
	 && (!group_explicitly_left || (X_MrAddToGrp&&strcasecmp(self_addr,X_MrAddToGrp)==0) ) /*re-create explicitly left groups only if ourself is re-added*/
	 )
	{
		stmt = mrsqlite3_prepare_v2_(mailbox->m_sql,
			"INSERT INTO chats (type, name, grpid) VALUES(?, ?, ?);");
		sqlite3_bind_int (stmt, 1, MR_CHAT_GROUP);
		sqlite3_bind_text(stmt, 2, grpname, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 3, grpid, -1, SQLITE_STATIC);
		if( sqlite3_step(stmt)!=SQLITE_DONE ) {
			goto cleanup;
		}
		sqlite3_finalize(stmt);
		chat_id = sqlite3_last_insert_rowid(mailbox->m_sql->m_cobj);
		recreate_member_list = 1;
	}

	/* again, check chat_id */
	if( chat_id <= MR_CHAT_ID_LAST_SPECIAL ) {
		chat_id = 0;
		if( group_explicitly_left ) {
			chat_id = MR_CHAT_ID_TRASH; /* we got a message for a chat we've deleted - do not show this even as a normal chat */
		}
		goto cleanup;
	}

	/* execute group commands */
	if( X_MrAddToGrp || X_MrRemoveFromGrp )
	{
		recreate_member_list = 1;
	}
	else if( X_MrGrpNameChanged && grpname && strlen(grpname) < 200 )
	{
		stmt = mrsqlite3_prepare_v2_(mailbox->m_sql, "UPDATE chats SET name=? WHERE id=?;");
		sqlite3_bind_text(stmt, 1, grpname, -1, SQLITE_STATIC);
		sqlite3_bind_int (stmt, 2, chat_id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		mailbox->m_cb(mailbox, MR_EVENT_CHAT_MODIFIED, chat_id, 0);
	}

	/* add members to group/check members
	for recreation: we should add a timestamp */
	if( recreate_member_list )
	{
		const char* skip = X_MrRemoveFromGrp? X_MrRemoveFromGrp : NULL;

		stmt = mrsqlite3_prepare_v2_(mailbox->m_sql, "DELETE FROM chats_contacts WHERE chat_id=?;");
		sqlite3_bind_int (stmt, 1, chat_id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);

		if( skip==NULL || strcasecmp(self_addr, skip) != 0 ) {
			mrmailbox_add_contact_to_chat__(mailbox, chat_id, MR_CONTACT_ID_SELF);
		}

		if( from_id > MR_CONTACT_ID_LAST_SPECIAL ) {
			if( mrmailbox_contact_addr_equals__(mailbox, from_id, self_addr)==0
			 && (skip==NULL || mrmailbox_contact_addr_equals__(mailbox, from_id, skip)==0) ) {
				mrmailbox_add_contact_to_chat__(mailbox, chat_id, from_id);
			}
		}

		for( i = 0; i < to_list_cnt; i++ )
		{
			uint32_t to_id = (uint32_t)(uintptr_t)carray_get(to_list, i); /* to_id is only once in to_list and is non-special */
			if( mrmailbox_contact_addr_equals__(mailbox, to_id, self_addr)==0
			 && (skip==NULL || mrmailbox_contact_addr_equals__(mailbox, to_id, skip)==0) ) {
				mrmailbox_add_contact_to_chat__(mailbox, chat_id, to_id);
			}
		}
		mailbox->m_cb(mailbox, MR_EVENT_CHAT_MODIFIED, chat_id, 0);
	}

	/* check the number of receivers -
	the only critical situation is if the user hits "Reply" instead of "Reply all" in a non-messenger-client */
	if( to_list_cnt == 1 && mime_parser->m_is_send_by_messenger==0 ) {
		int is_contact_cnt = mrmailbox_get_chat_contact_count__(mailbox, chat_id);
		if( is_contact_cnt > 3 /* to_list_cnt==1 may be "From: A, To: B, SELF" as SELF is not counted in to_list_cnt. So everything up to 3 is no error. */ ) {
			chat_id = 0;
			goto cleanup;
		}
	}

cleanup:
	free(grpid1);
	free(grpid2);
	free(grpid3);
	free(grpid4);
	free(grpname);
	free(self_addr);
	return chat_id;
}


/*******************************************************************************
 * Receive a message and add it to the database
 ******************************************************************************/


static time_t correct_bad_timestamp__(mrmailbox_t* ths, uint32_t chat_id, uint32_t from_id, time_t desired_timestamp, int is_fresh_msg)
{
	/* use the last message from another user (including SELF) as the MINIMUM
	(we do this check only for fresh messages, other messages may pop up whereever, this may happen eg. when restoring old messages or synchronizing different clients) */
	if( is_fresh_msg )
	{
		sqlite3_stmt* stmt = mrsqlite3_predefine__(ths->m_sql, SELECT_timestamp_FROM_msgs_WHERE_timestamp,
			"SELECT MAX(timestamp) FROM msgs WHERE chat_id=? and from_id!=? AND timestamp>=?");
		sqlite3_bind_int  (stmt,  1, chat_id);
		sqlite3_bind_int  (stmt,  2, from_id);
		sqlite3_bind_int64(stmt,  3, desired_timestamp);
		if( sqlite3_step(stmt)==SQLITE_ROW )
		{
			time_t last_msg_time = sqlite3_column_int64(stmt, 0);
			if( last_msg_time > 0 /* may happen as we do not check against sqlite3_column_type()!=SQLITE_NULL */ ) {
				if( desired_timestamp <= last_msg_time ) {
					desired_timestamp = last_msg_time+1; /* this may result in several incoming messages having the same
					                                     one-second-after-the-last-other-message-timestamp.  however, this is no big deal
					                                     as we do not try to recrete the order of bad-date-messages and as we always order by ID as second criterion */
				}
			}
		}
	}

	/* use the (smeared) current time as the MAXIMUM */
	if( desired_timestamp >= mr_smeared_time__() )
	{
		desired_timestamp = mr_create_smeared_timestamp__();
	}

	return desired_timestamp;
}


static void add_or_lookup_contact_by_addr__(mrmailbox_t* ths, const char* display_name_enc, const char* addr_spec, int origin, carray* ids, int* check_self)
{
	/* is addr_spec equal to SELF? */
	int dummy;
	if( check_self == NULL ) { check_self = &dummy; }

	*check_self = 0;

	char* self_addr = mrsqlite3_get_config__(ths->m_sql, "configured_addr", "");
		if( strcasecmp(self_addr, addr_spec)==0 ) {
			*check_self = 1;
		}
	free(self_addr);

	if( *check_self ) {
		return;
	}

	/* add addr_spec if missing, update otherwise */
	char* display_name_dec = NULL;
	if( display_name_enc ) {
		display_name_dec = mr_decode_header_string(display_name_enc);
		mr_normalize_name(display_name_dec);
	}

	uint32_t row_id = mrmailbox_add_or_lookup_contact__(ths, display_name_dec /*can be NULL*/, addr_spec, origin, NULL);

	free(display_name_dec);

	if( row_id ) {
		if( !carray_search(ids, (void*)(uintptr_t)row_id, NULL) ) {
			carray_add(ids, (void*)(uintptr_t)row_id, NULL);
		}
	}
}


static void add_or_lookup_contacts_by_mailbox_list__(mrmailbox_t* ths, struct mailimf_mailbox_list* mb_list, int origin, carray* ids, int* check_self)
{
	clistiter* cur;
	for( cur = clist_begin(mb_list->mb_list); cur!=NULL ; cur=clist_next(cur) ) {
		struct mailimf_mailbox* mb = (struct mailimf_mailbox*)clist_content(cur);
		if( mb ) {
			add_or_lookup_contact_by_addr__(ths, mb->mb_display_name, mb->mb_addr_spec, origin, ids, check_self);
		}
	}
}


static void add_or_lookup_contacts_by_address_list__(mrmailbox_t* ths, struct mailimf_address_list* adr_list, int origin, carray* ids, int* check_self)
{
	clistiter* cur;
	for( cur = clist_begin(adr_list->ad_list); cur!=NULL ; cur=clist_next(cur) ) {
		struct mailimf_address* adr = (struct mailimf_address*)clist_content(cur);
		if( adr ) {
			if( adr->ad_type == MAILIMF_ADDRESS_MAILBOX ) {
				struct mailimf_mailbox* mb = adr->ad_data.ad_mailbox; /* can be NULL */
				if( mb ) {
					add_or_lookup_contact_by_addr__(ths, mb->mb_display_name, mb->mb_addr_spec, origin, ids, check_self);
				}
			}
			else if( adr->ad_type == MAILIMF_ADDRESS_GROUP ) {
				struct mailimf_group* group = adr->ad_data.ad_group; /* can be NULL */
				if( group && group->grp_mb_list /*can be NULL*/ ) {
					add_or_lookup_contacts_by_mailbox_list__(ths, group->grp_mb_list, origin, ids, check_self);
				}
			}
		}
	}
}


static int is_known_rfc724_mid__(mrmailbox_t* mailbox, const char* rfc724_mid)
{
	if( rfc724_mid ) {
		sqlite3_stmt* stmt = mrsqlite3_predefine__(mailbox->m_sql, SELECT_id_FROM_msgs_WHERE_cm,
			"SELECT id FROM msgs WHERE rfc724_mid=? AND (chat_id>? OR from_id=?);");
		sqlite3_bind_text(stmt, 1, rfc724_mid, -1, SQLITE_STATIC);
		sqlite3_bind_int (stmt, 2, MR_CHAT_ID_LAST_SPECIAL);
		sqlite3_bind_int (stmt, 3, MR_CONTACT_ID_SELF);
		if( sqlite3_step(stmt) == SQLITE_ROW ) {
			return 1;
		}
	}
	return 0;
}


static int is_known_rfc724_mid_in_list__(mrmailbox_t* mailbox, const clist* mid_list)
{
	if( mid_list ) {
		clistiter* cur;
		for( cur = clist_begin(mid_list); cur!=NULL ; cur=clist_next(cur) ) {
			if( is_known_rfc724_mid__(mailbox, clist_content(cur)) ) {
				return 1;
			}
		}
	}

	return 0;
}


static int is_reply_to_known_message__(mrmailbox_t* mailbox, mrmimeparser_t* mime_parser)
{
	/* check if the message is a reply to a known message; the replies are identified by the Message-ID from
	`In-Reply-To`/`References:` (to support non-Delta-Clients) or from `X-MrPredecessor:` (Delta clients, see comment in mrchat.c) */
	clistiter* cur;
	for( cur = clist_begin(mime_parser->m_header->fld_list); cur!=NULL ; cur=clist_next(cur) )
	{
		struct mailimf_field* field = (struct mailimf_field*)clist_content(cur);
		if( field )
		{
			if( field->fld_type == MAILIMF_FIELD_OPTIONAL_FIELD )
			{
				struct mailimf_optional_field* optional_field = field->fld_data.fld_optional_field;
				if( optional_field && optional_field->fld_name ) {
					if( strcasecmp(optional_field->fld_name, "X-MrPredecessor")==0 || strcasecmp(optional_field->fld_name, "Chat-Predecessor")==0 ) { /* see comment in mrchat.c */
						if( is_known_rfc724_mid__(mailbox, optional_field->fld_value) ) {
							return 1;
						}
					}
				}
			}
			else if( field->fld_type == MAILIMF_FIELD_IN_REPLY_TO )
			{
				struct mailimf_in_reply_to* fld_in_reply_to = field->fld_data.fld_in_reply_to;
				if( fld_in_reply_to ) {
					if( is_known_rfc724_mid_in_list__(mailbox, field->fld_data.fld_in_reply_to->mid_list) ) {
						return 1;
					}
				}
			}
			else if( field->fld_type == MAILIMF_FIELD_REFERENCES )
			{
				struct mailimf_references* fld_references = field->fld_data.fld_references;
				if( fld_references ) {
					if( is_known_rfc724_mid_in_list__(mailbox, field->fld_data.fld_references->mid_list) ) {
						return 1;
					}
				}
			}

		}
	}

	return 0;
}


static void receive_imf(mrmailbox_t* ths, const char* imf_raw_not_terminated, size_t imf_raw_bytes,
                          const char* server_folder, uint32_t server_uid, uint32_t flags)
{
	/* the function returns the number of created messages in the database */
	int              incoming = 0;
	int              incoming_from_known_sender = 0;
	#define          outgoing (!incoming)
	int              is_group = 0;

	carray*          to_list = NULL;

	uint32_t         from_id = 0;
	int              from_id_blocked = 0;
	uint32_t         to_id   = 0;
	uint32_t         chat_id = 0;
	int              state   = MR_STATE_UNDEFINED;

	sqlite3_stmt*    stmt;
	size_t           i, icnt;
	uint32_t         first_dblocal_id = 0;
	char*            rfc724_mid = NULL; /* Message-ID from the header */
	time_t           message_timestamp = MR_INVALID_TIMESTAMP;
	mrmimeparser_t*  mime_parser = mrmimeparser_new(ths->m_blobdir, ths);
	int              db_locked = 0;
	int              transaction_pending = 0;
	clistiter*       cur1;
	const struct mailimf_field* field;

	carray*          created_db_entries = carray_new(16);
	int              create_event_to_send = MR_EVENT_MSGS_CHANGED;

	carray*          rr_event_to_send = carray_new(16);

	int              has_return_path = 0;
	char*            txt_raw = NULL;

	to_list = carray_new(16);
	if( to_list==NULL || created_db_entries==NULL || rr_event_to_send==NULL || mime_parser == NULL ) {
		goto cleanup;
	}

	/* parse the imf to mailimf_message {
	        mailimf_fields* msg_fields {
	          clist* fld_list; // list of mailimf_field
	        }
	        mailimf_body* msg_body { // != NULL
                const char * bd_text; // != NULL
                size_t bd_size;
	        }
	   };
	normally, this is done by mailimf_message_parse(), however, as we also need the MIME data,
	we use mailmime_parse() through MrMimeParser (both call mailimf_struct_multiple_parse() somewhen, I did not found out anything
	that speaks against this approach yet) */
	mrmimeparser_parse(mime_parser, imf_raw_not_terminated, imf_raw_bytes);
	if( mime_parser->m_header == NULL ) {
		goto cleanup; /* Error - even adding an empty record won't help as we do not know the message ID */
	}

	mrsqlite3_lock(ths->m_sql);
	db_locked = 1;

	mrsqlite3_begin_transaction__(ths->m_sql);
	transaction_pending = 1;


		/* Check, if the mail comes from extern, resp. is not send by us.  This is a _really_ important step
		as messages send by us are used to validate other mail senders and receivers.
		For this purpose, we assume, the `Return-Path:`-header is never present if the message is send by us.
		The `Received:`-header may be another idea, however, this is also set if mails are transfered from other accounts via IMAP.
		Using `From:` alone is no good idea, as mailboxes may use different sending-addresses - moreover, they may change over the years.
		However, we use `From:` as an additional hint below. */
		for( cur1 = clist_begin(mime_parser->m_header->fld_list); cur1!=NULL ; cur1=clist_next(cur1) )
		{
			field = (struct mailimf_field*)clist_content(cur1);
			if( field )
			{
				if( field->fld_type == MAILIMF_FIELD_RETURN_PATH )
				{
					has_return_path = 1;
				}
				else if( field->fld_type == MAILIMF_FIELD_OPTIONAL_FIELD )
				{
					struct mailimf_optional_field* optional_field = field->fld_data.fld_optional_field;
					if( optional_field && strcasecmp(optional_field->fld_name, "Return-Path")==0 )
					{
						has_return_path = 1; /* "MAILIMF_FIELD_OPTIONAL_FIELD.Return-Path" should be "MAILIMF_FIELD_RETURN_PATH", however, this is not always the case */
					}
				}
			}
		}

		if( has_return_path ) {
			incoming = 1;
		}


		/* for incoming messages, get From: and check if it is known (for known From:'s we add the other To:/Cc:/Bcc: in the 3rd pass) */
		if( incoming
		 && (field=mr_find_mailimf_field(mime_parser->m_header,  MAILIMF_FIELD_FROM  ))!=NULL )
		{
			struct mailimf_from* fld_from = field->fld_data.fld_from;
			if( fld_from )
			{
				int check_self;
				carray* from_list = carray_new(16);
				add_or_lookup_contacts_by_mailbox_list__(ths, fld_from->frm_mb_list, MR_ORIGIN_INCOMING_UNKNOWN_FROM, from_list, &check_self);
				if( check_self )
				{
					incoming = 0; /* The `Return-Path:`-approach above works well, however, there may be messages outgoing messages which we also receive -
					              for these messages, the `Return-Path:` is set although we're the sender.  To correct these cases, we add an
					              additional From: check - which, however, will not work for older From:-addresses used on the mailbox. */
				}
				else
				{
					if( carray_count(from_list)>=1 ) /* if there is no from given, from_id stays 0 which is just fine.  These messages are very rare, however, we have to add the to the database (they to to the "deaddrop" chat) to avoid a re-download from the server. See also [**] */
					{
						from_id = (uint32_t)(uintptr_t)carray_get(from_list, 0);
						if( mrmailbox_is_known_contact__(ths, from_id, &from_id_blocked) ) { /* currently, this checks if the contact is non-blocked and is known by any reason, we could be more strict and allow eg. only contacts already used for sending. However, as a first idea, the current approach seems okay. */
							incoming_from_known_sender = 1;
						}
					}
				}
				carray_free(from_list);
			}
		}

		/* Make sure, to_list starts with the first To:-address (Cc: and Bcc: are added in the loop below pass) */
		if( (outgoing || incoming_from_known_sender)
		 && (field=mr_find_mailimf_field(mime_parser->m_header,  MAILIMF_FIELD_TO  ))!=NULL )
		{
			struct mailimf_to* fld_to = field->fld_data.fld_to; /* can be NULL */
			if( fld_to )
			{
				add_or_lookup_contacts_by_address_list__(ths, fld_to->to_addr_list /*!= NULL*/,
					outgoing? MR_ORIGIN_OUTGOING_TO : MR_ORIGIN_INCOMING_TO, to_list, NULL);
			}
		}


		if( carray_count(mime_parser->m_parts) > 0 )
		{

			/**********************************************************************
			 * Add parts
			 *********************************************************************/

			/* collect the rest information */
			for( cur1 = clist_begin(mime_parser->m_header->fld_list); cur1!=NULL ; cur1=clist_next(cur1) )
			{
				field = (struct mailimf_field*)clist_content(cur1);
				if( field )
				{
					if( field->fld_type == MAILIMF_FIELD_MESSAGE_ID )
					{
						struct mailimf_message_id* fld_message_id = field->fld_data.fld_message_id;
						if( fld_message_id ) {
							rfc724_mid = safe_strdup(fld_message_id->mid_value);
						}
					}
					else if( field->fld_type == MAILIMF_FIELD_CC )
					{
						struct mailimf_cc* fld_cc = field->fld_data.fld_cc;
						if( fld_cc ) {
							add_or_lookup_contacts_by_address_list__(ths, fld_cc->cc_addr_list,
								outgoing? MR_ORIGIN_OUTGOING_CC : MR_ORIGIN_INCOMING_CC, to_list, NULL);
						}
					}
					else if( field->fld_type == MAILIMF_FIELD_BCC )
					{
						struct mailimf_bcc* fld_bcc = field->fld_data.fld_bcc;
						if( outgoing && fld_bcc ) {
							add_or_lookup_contacts_by_address_list__(ths, fld_bcc->bcc_addr_list,
								MR_ORIGIN_OUTGOING_BCC, to_list, NULL);
						}
					}
					else if( field->fld_type == MAILIMF_FIELD_ORIG_DATE )
					{
						struct mailimf_orig_date* orig_date = field->fld_data.fld_orig_date;
						if( orig_date ) {
							message_timestamp = mr_timestamp_from_date(orig_date->dt_date_time); /* is not yet checked against bad times! */
						}
					}
				}

			} /* for */


			/* check if the message introduces a new chat:
			- outgoing messages introduce a chat with the first to: address if they are send by a messenger
			- incoming messages introduce a chat only for known contacts if they are send by a messenger
			(of course, the user can add other chats manually later) */
			if( incoming )
			{
				state = (flags&MR_IMAP_SEEN)? MR_IN_SEEN : MR_IN_FRESH;
				to_id = MR_CONTACT_ID_SELF;

				chat_id = lookup_group_by_grpid__(ths, mime_parser,
					(incoming_from_known_sender && mime_parser->m_is_send_by_messenger)/*create as needed?*/, from_id, to_list);
				if( chat_id )
				{
					is_group = 1;
				}
				else
				{
					chat_id = mrmailbox_lookup_real_nchat_by_contact_id__(ths, from_id);
					if( chat_id == 0 )
					{
						if( incoming_from_known_sender && mime_parser->m_is_send_by_messenger ) {
							chat_id = mrmailbox_create_or_lookup_nchat_by_contact_id__(ths, from_id);
						}
						else if( is_reply_to_known_message__(ths, mime_parser) ) {
							mrmailbox_scaleup_contact_origin__(ths, from_id, MR_ORIGIN_INCOMING_REPLY_TO);
							chat_id = mrmailbox_create_or_lookup_nchat_by_contact_id__(ths, from_id);
						}
					}

					if( chat_id == 0 ) {
						chat_id = MR_CHAT_ID_DEADDROP;
					}
				}
			}
			else /* outgoing */
			{
				state = MR_OUT_DELIVERED; /* the mail is on the IMAP server, probably it is also deliverd.  We cannot recreate other states (read, error). */
				from_id = MR_CONTACT_ID_SELF;
				if( carray_count(to_list) >= 1 ) {
					to_id   = (uint32_t)(uintptr_t)carray_get(to_list, 0);

					chat_id = lookup_group_by_grpid__(ths, mime_parser, true/*create as needed*/, from_id, to_list);
					if( chat_id )
					{
						is_group = 1;
					}
					else
					{
						chat_id = mrmailbox_lookup_real_nchat_by_contact_id__(ths, to_id);
						if( chat_id == 0 && mime_parser->m_is_send_by_messenger && !mrmailbox_is_contact_blocked__(ths, to_id) ) {
							chat_id = mrmailbox_create_or_lookup_nchat_by_contact_id__(ths, to_id);
						}
					}
				}

				if( chat_id == 0 ) {
					chat_id = MR_CHAT_ID_TO_DEADDROP;
				}
			}

			/* correct message_timestamp, it should not be used before,
			however, we cannot do this earlier as we need from_id to be set */
			message_timestamp = correct_bad_timestamp__(ths, chat_id, from_id, message_timestamp, (flags&MR_IMAP_SEEN)? 0 : 1 /*fresh message?*/);

			/* check, if the mail is already in our database - if so, there's nothing more to do
			(we may get a mail twice eg. it it is moved between folders) */
			if( rfc724_mid == NULL ) {
				/* header is lacking a Message-ID - this may be the case, if the message was sent from this account and the mail client
				the the SMTP-server set the ID (true eg. for the Webmailer used in all-inkl-KAS)
				in these cases, we build a message ID based on some useful header fields that do never change (date, to)
				we do not use the folder-local id, as this will change if the mail is moved to another folder. */
				rfc724_mid = mr_create_incoming_rfc724_mid(message_timestamp, from_id, to_list);
				if( rfc724_mid == NULL ) {
					goto cleanup;
				}
			}

			{
				char*    old_server_folder = NULL;
				uint32_t old_server_uid = 0;
				if( mrmailbox_rfc724_mid_exists__(ths, rfc724_mid, &old_server_folder, &old_server_uid) ) {
					/* The message is already added to our database; rollback.  If needed update the server_uid which may have changed if the message was moved around on the server. */
					if( strcmp(old_server_folder, server_folder)!=0 || old_server_uid!=server_uid ) {
						mrsqlite3_rollback__(ths->m_sql);
						transaction_pending = 0;
						mrmailbox_update_server_uid__(ths, rfc724_mid, server_folder, server_uid);
					}
					free(old_server_folder);
					goto cleanup;
				}
			}

			/* fine, so far.  now, split the message into simple parts usable as "short messages"
			and add them to the database (mails send by other messenger clients should result
			into only one message; mails send by other clients may result in several messages (eg. one per attachment)) */
			icnt = carray_count(mime_parser->m_parts); /* should be at least one - maybe empty - part */
			for( i = 0; i < icnt; i++ )
			{
				mrmimepart_t* part = (mrmimepart_t*)carray_get(mime_parser->m_parts, i);

				if( part->m_type == MR_MSG_TEXT ) {
					txt_raw = mr_mprintf("%s\n\n%s", mime_parser->m_subject? mime_parser->m_subject : "", part->m_msg_raw);
				}

				stmt = mrsqlite3_predefine__(ths->m_sql, INSERT_INTO_msgs_msscftttsmttpb,
					"INSERT INTO msgs (rfc724_mid,server_folder,server_uid,chat_id,from_id, to_id,timestamp,type, state,msgrmsg,txt,txt_raw,param,bytes)"
					" VALUES (?,?,?,?,?, ?,?,?, ?,?,?,?,?,?);");
				sqlite3_bind_text (stmt,  1, rfc724_mid, -1, SQLITE_STATIC);
				sqlite3_bind_text (stmt,  2, server_folder, -1, SQLITE_STATIC);
				sqlite3_bind_int  (stmt,  3, server_uid);
				sqlite3_bind_int  (stmt,  4, chat_id);
				sqlite3_bind_int  (stmt,  5, from_id);
				sqlite3_bind_int  (stmt,  6, to_id);
				sqlite3_bind_int64(stmt,  7, message_timestamp);
				sqlite3_bind_int  (stmt,  8, part->m_type);
				sqlite3_bind_int  (stmt,  9, state);
				sqlite3_bind_int  (stmt, 10, mime_parser->m_is_send_by_messenger);
				sqlite3_bind_text (stmt, 11, part->m_msg? part->m_msg : "", -1, SQLITE_STATIC);
				sqlite3_bind_text (stmt, 12, txt_raw? txt_raw : "", -1, SQLITE_STATIC);
				sqlite3_bind_text (stmt, 13, part->m_param->m_packed, -1, SQLITE_STATIC);
				sqlite3_bind_int  (stmt, 14, part->m_bytes);
				if( sqlite3_step(stmt) != SQLITE_DONE ) {
					goto cleanup; /* i/o error - there is nothing more we can do - in other cases, we try to write at least an empty record */
				}

				free(txt_raw);
				txt_raw = NULL;

				if( first_dblocal_id == 0 ) {
					first_dblocal_id = sqlite3_last_insert_rowid(ths->m_sql->m_cobj);
				}

				carray_add(created_db_entries, (void*)(uintptr_t)chat_id, NULL);
				carray_add(created_db_entries, (void*)(uintptr_t)first_dblocal_id, NULL);
			}

			/* finally, create "ghost messages" for additional to:, cc: bcc: receivers
			(just to be more compatibe to standard email-programs, the flow in the Messanger would not need this) */
			if( outgoing && is_group == 0 && carray_count(to_list)>1 && first_dblocal_id != 0 )
			{
				char* ghost_rfc724_mid_str = mr_mprintf(MR_GHOST_ID_FORMAT, first_dblocal_id); /* G@id is used to find the message if the original is deleted */
				char* ghost_param = mr_mprintf("G=%lu", first_dblocal_id);                    /* G=Ghost message flag with the original message ID */
				char* ghost_txt = NULL;
				{
					mrmimepart_t* part = (mrmimepart_t*)carray_get(mime_parser->m_parts, 0);
					ghost_txt = mrmsg_get_summarytext_by_raw(part->m_type, part->m_msg, part->m_param, APPROX_SUBJECT_CHARS);
				}

				icnt = carray_count(to_list);
				for( i = 1/*the first one is added in detail above*/; i < icnt; i++ )
				{
					uint32_t ghost_to_id   = (uint32_t)(uintptr_t)carray_get(to_list, i);
					uint32_t ghost_chat_id = mrmailbox_lookup_real_nchat_by_contact_id__(ths, ghost_to_id);
					uint32_t ghost_dblocal_id;
					if( ghost_chat_id==0 ) {
						ghost_chat_id = MR_CHAT_ID_TO_DEADDROP;
					}

					stmt = mrsqlite3_predefine__(ths->m_sql, INSERT_INTO_msgs_msscftttsmttpb, NULL /*the first_dblocal_id-check above makes sure, the query is really created*/);
					sqlite3_bind_text (stmt,  1, ghost_rfc724_mid_str, -1, SQLITE_STATIC);
					sqlite3_bind_text (stmt,  2, "", -1, SQLITE_STATIC);
					sqlite3_bind_int  (stmt,  3, 0);
					sqlite3_bind_int  (stmt,  4, ghost_chat_id);
					sqlite3_bind_int  (stmt,  5, from_id);
					sqlite3_bind_int  (stmt,  6, ghost_to_id);
					sqlite3_bind_int64(stmt,  7, message_timestamp);
					sqlite3_bind_int  (stmt,  8, MR_MSG_TEXT);
					sqlite3_bind_int  (stmt,  9, state);
					sqlite3_bind_int  (stmt, 10, mime_parser->m_is_send_by_messenger);
					sqlite3_bind_text (stmt, 11, ghost_txt, -1, SQLITE_STATIC);
					sqlite3_bind_text (stmt, 12, "", -1, SQLITE_STATIC);
					sqlite3_bind_text (stmt, 13, ghost_param, -1, SQLITE_STATIC);
					sqlite3_bind_int  (stmt, 14, 0);
					if( sqlite3_step(stmt) != SQLITE_DONE ) {
						goto cleanup; /* i/o error - there is nothing more we can do - in other cases, we try to write at least an empty record */
					}

					ghost_dblocal_id = sqlite3_last_insert_rowid(ths->m_sql->m_cobj);

					carray_add(created_db_entries, (void*)(uintptr_t)ghost_chat_id, NULL);
					carray_add(created_db_entries, (void*)(uintptr_t)ghost_dblocal_id, NULL);
				}
				free(ghost_txt);
				free(ghost_param);
				free(ghost_rfc724_mid_str);
			}

			/* check event to send */
			if( incoming && state==MR_IN_FRESH )
			{
				if( from_id_blocked ) {
					create_event_to_send = 0;
				}
				else if( chat_id == MR_CHAT_ID_DEADDROP ) {
					if( mrsqlite3_get_config_int__(ths->m_sql, "show_deaddrop", 0)!=0 ) {
						create_event_to_send = MR_EVENT_INCOMING_MSG;
					}
				}
				else {
					create_event_to_send = MR_EVENT_INCOMING_MSG;
				}
			}

		}


		if( carray_count(mime_parser->m_reports) > 0 )
		{
			/******************************************************************
			 * Handle reports (mainly MDNs)
			 *****************************************************************/

			int mdns_enabled = mrsqlite3_get_config_int__(ths->m_sql, "mdns_enabled", MR_MDNS_DEFAULT_ENABLED);
			icnt = carray_count(mime_parser->m_reports);
			for( i = 0; i < icnt; i++ )
			{
				struct mailmime*           report_root = carray_get(mime_parser->m_reports, i);
				struct mailmime_parameter* report_type = mr_find_ct_parameter(report_root, "report-type");
				if( report_root==NULL || report_type==NULL || report_type->pa_value==NULL ) {
					continue;
				}

				if( strcmp(report_type->pa_value, "disposition-notification") == 0
				 && clist_count(report_root->mm_data.mm_multipart.mm_mp_list) >= 2 /* the first part is for humans, the second for machines */
				 && mdns_enabled /*to get a clear functionality, do not show incoming MDNs if the options is disabled*/ )
				{
					struct mailmime* report_data = (struct mailmime*)clist_content(clist_next(clist_begin(report_root->mm_data.mm_multipart.mm_mp_list)));
					if( report_data
					 && report_data->mm_content_type->ct_type->tp_type==MAILMIME_TYPE_COMPOSITE_TYPE
					 && report_data->mm_content_type->ct_type->tp_data.tp_composite_type->ct_type==MAILMIME_COMPOSITE_TYPE_MESSAGE
					 && strcmp(report_data->mm_content_type->ct_subtype, "disposition-notification")==0 )
					{
						/* we received a MDN (although the MDN is only a header, we parse it as a complete mail) */
						const char* report_body = NULL;
						size_t      report_body_bytes = 0;
						char*       to_mmap_string_unref = NULL;
						if( mr_mime_transfer_decode(report_data, &report_body, &report_body_bytes, &to_mmap_string_unref) )
						{
							struct mailmime* report_parsed = NULL;
							size_t dummy = 0;
							if( mailmime_parse(report_body, report_body_bytes, &dummy, &report_parsed)==MAIL_NO_ERROR
							 && report_parsed!=NULL )
							{
								struct mailimf_fields* report_fields = mr_find_mailimf_fields(report_parsed);
								if( report_fields )
								{
									struct mailimf_optional_field* of_disposition = mr_find_mailimf_field2(report_fields, "Disposition"); /* MUST be preset, _if_ preset, we assume a sort of attribution and do not go into details */
									struct mailimf_optional_field* of_org_msgid   = mr_find_mailimf_field2(report_fields, "Original-Message-ID"); /* can't live without */
									if( of_disposition && of_disposition->fld_value && of_org_msgid && of_org_msgid->fld_value )
									{
										char* rfc724_mid = NULL;
										dummy = 0;
										if( mailimf_msg_id_parse(of_org_msgid->fld_value, strlen(of_org_msgid->fld_value), &dummy, &rfc724_mid)==MAIL_NO_ERROR
										 && rfc724_mid!=NULL )
										{
											uint32_t chat_id = 0;
											uint32_t msg_id = 0;
											if( mrmailbox_mdn_from_ext__(ths, from_id, rfc724_mid, &chat_id, &msg_id) ) {
												carray_add(rr_event_to_send, (void*)(uintptr_t)chat_id, NULL);
												carray_add(rr_event_to_send, (void*)(uintptr_t)msg_id, NULL);
											}
											free(rfc724_mid);
										}
									}
								}
								mailmime_free(report_parsed);
							}

							if( to_mmap_string_unref ) { mmap_string_unref(to_mmap_string_unref); }
						}
					}
				}

			}

		}

	/* end sql-transaction */
	mrsqlite3_commit__(ths->m_sql);
	transaction_pending = 0;

	#if 0
	{
		mrsqlite3_lock(ths->m_sql);
			char* debugDir = mrsqlite3_get_config_(ths->m_sql, "debug_dir", NULL);
			if( debugDir ) {
				char filename[512];
				snprintf(filename, sizeof(filename), "%s/%s-%i.eml", debugDir, server_folder, (int)first_dblocal_id);
				FILE* f = fopen(filename, "w");
				if( f ) {
					fwrite(imf_raw_not_terminated, 1, imf_raw_bytes, f);
					fclose(f);
				}
				free(debugDir);
			}
		mrsqlite3_unlock(ths->m_sql);
	}
	#endif

	/* done */
cleanup:
	if( transaction_pending ) {
		mrsqlite3_rollback__(ths->m_sql);
	}

	if( db_locked ) {
		mrsqlite3_unlock(ths->m_sql);
	}

	if( mime_parser ) {
		mrmimeparser_unref(mime_parser);
	}

	if( rfc724_mid ) {
		free(rfc724_mid);
	}

	if( to_list ) {
		carray_free(to_list);
	}

	if( created_db_entries ) {
		if( create_event_to_send ) {
			size_t i, icnt = carray_count(created_db_entries);
			for( i = 0; i < icnt; i += 2 ) {
				ths->m_cb(ths, create_event_to_send, (uintptr_t)carray_get(created_db_entries, i), (uintptr_t)carray_get(created_db_entries, i+1));
			}
		}
		carray_free(created_db_entries);
	}

	if( rr_event_to_send ) {
		size_t i, icnt = carray_count(rr_event_to_send);
		for( i = 0; i < icnt; i += 2 ) {
			ths->m_cb(ths, MR_EVENT_MSG_READ, (uintptr_t)carray_get(rr_event_to_send, i), (uintptr_t)carray_get(rr_event_to_send, i+1));
		}
		carray_free(rr_event_to_send);
	}

	free(txt_raw);
}


/*******************************************************************************
 * Main interface
 ******************************************************************************/


static uintptr_t cb_dummy(mrmailbox_t* mailbox, int event, uintptr_t data1, uintptr_t data2)
{
	return 0;
}
static int32_t cb_get_config_int(mrimap_t* imap, const char* key, int32_t value)
{
	mrmailbox_t* mailbox = (mrmailbox_t*)imap->m_userData;
	mrsqlite3_lock(mailbox->m_sql);
		int32_t ret = mrsqlite3_get_config_int__(mailbox->m_sql, key, value);
	mrsqlite3_unlock(mailbox->m_sql);
	return ret;
}
static void cb_set_config_int(mrimap_t* imap, const char* key, int32_t def)
{
	mrmailbox_t* mailbox = (mrmailbox_t*)imap->m_userData;
	mrsqlite3_lock(mailbox->m_sql);
		mrsqlite3_set_config_int__(mailbox->m_sql, key, def);
	mrsqlite3_unlock(mailbox->m_sql);
}
static void cb_receive_imf(mrimap_t* imap, const char* imf_raw_not_terminated, size_t imf_raw_bytes, const char* server_folder, uint32_t server_uid, uint32_t flags)
{
	mrmailbox_t* mailbox = (mrmailbox_t*)imap->m_userData;
	receive_imf(mailbox, imf_raw_not_terminated, imf_raw_bytes, server_folder, server_uid, flags);
}


mrmailbox_t* mrmailbox_new(mrmailboxcb_t cb, void* userData)
{
	mrmailbox_get_thread_index(); /* make sure, the main thread has the index #1, only for a nicer look of the logs */

	mrmailbox_t* ths = NULL;

	if( (ths=calloc(1, sizeof(mrmailbox_t)))==NULL ) {
		exit(23); /* cannot allocate little memory, unrecoverable error */
	}

	pthread_mutex_init(&ths->m_wake_lock_critical, NULL);

	ths->m_sql      = mrsqlite3_new(ths);
	ths->m_cb       = cb? cb : cb_dummy;
	ths->m_userData = userData;
	ths->m_imap     = mrimap_new(cb_get_config_int, cb_set_config_int, cb_receive_imf, (void*)ths, ths);
	ths->m_smtp     = mrsmtp_new(ths);

	mrjob_init_thread(ths);

	mrpgp_init(ths);

	/* Random-seed.  An additional seed with more random data is done just before key generation
	(the timespan between this call and the key generation time is typically random.
	Moreover, later, we add a hash of the first message data to the random-seed
	(it would be okay to seed with even more sensible data, the seed values cannot be recovered from the PRNG output, see OpenSSL's RAND_seed() ) */
	{
	uintptr_t seed[5];
	seed[0] = (uintptr_t)time(NULL);     /* time */
	seed[1] = (uintptr_t)seed;           /* stack */
	seed[2] = (uintptr_t)ths;            /* heap */
	seed[3] = (uintptr_t)pthread_self(); /* thread ID */
	seed[4] = (uintptr_t)getpid();       /* process ID */
	mrpgp_rand_seed(ths, seed, sizeof(seed));
	}

	if( s_localize_mb_obj==NULL ) {
		s_localize_mb_obj = ths;
	}

	return ths;
}


void mrmailbox_unref(mrmailbox_t* ths)
{
	if( ths==NULL ) {
		return;
	}

	mrpgp_exit(ths);

	mrjob_exit_thread(ths);

	if( mrmailbox_is_open(ths) ) {
		mrmailbox_close(ths);
	}

	mrimap_unref(ths->m_imap);
	mrsmtp_unref(ths->m_smtp);
	mrsqlite3_unref(ths->m_sql);
	pthread_mutex_destroy(&ths->m_wake_lock_critical);
	free(ths);

	if( s_localize_mb_obj==ths ) {
		s_localize_mb_obj = NULL;
	}
}


int mrmailbox_open(mrmailbox_t* ths, const char* dbfile, const char* blobdir)
{
	int success = 0;
	int db_locked = 0;

	if( ths == NULL || dbfile == NULL ) {
		goto cleanup;
	}

	mrsqlite3_lock(ths->m_sql);
	db_locked = 1;

	/* Open() sets up the object and connects to the given database
	from which all configuration is read/written to. */

	/* Create/open sqlite database */
	if( !mrsqlite3_open__(ths->m_sql, dbfile) ) {
		goto cleanup;
	}
	mrjob_kill_action__(ths, MRJ_CONNECT_TO_IMAP);

	/* backup dbfile name */
	ths->m_dbfile = safe_strdup(dbfile);

	/* set blob-directory
	(to avoid double slashed, the given directory should not end with an slash) */
	if( blobdir && blobdir[0] ) {
		ths->m_blobdir = safe_strdup(blobdir);
	}
	else {
		ths->m_blobdir = mr_mprintf("%s-blobs", dbfile);
		mr_create_folder(ths->m_blobdir, ths);
	}

	/* success */
	success = 1;

	/* cleanup */
cleanup:
	if( !success ) {
		if( mrsqlite3_is_open(ths->m_sql) ) {
			mrsqlite3_close__(ths->m_sql);
		}
	}

	if( db_locked ) {
		mrsqlite3_unlock(ths->m_sql);
	}

	return success;
}


void mrmailbox_close(mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	mrimap_disconnect(ths->m_imap);
	mrsmtp_disconnect(ths->m_smtp);

	mrsqlite3_lock(ths->m_sql);

		if( mrsqlite3_is_open(ths->m_sql) ) {
			mrsqlite3_close__(ths->m_sql);
		}

		free(ths->m_dbfile);
		ths->m_dbfile = NULL;

		free(ths->m_blobdir);
		ths->m_blobdir = NULL;

	mrsqlite3_unlock(ths->m_sql);
}


int mrmailbox_is_open(const mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return 0; /* error - database not opened */
	}

	return mrsqlite3_is_open(ths->m_sql);
}


int mrmailbox_poke_eml_file(mrmailbox_t* ths, const char* filename)
{
	/* mainly for testing, may be called by mrmailbox_import_spec() */
	int     success = 0;
	char*   data = NULL;
	size_t  data_bytes;

	if( ths == NULL ) {
		return 0;
	}

	if( mr_read_file(filename, (void**)&data, &data_bytes, ths) == 0 ) {
		goto cleanup;
	}

	receive_imf(ths, data, data_bytes, "import", 0, 0); /* this static function is the reason why this function is not moved to mrmailbox_imex.c */
	success = 1;

cleanup:
	free(data);

	return success;
}


/*******************************************************************************
 * INI-handling, Information
 ******************************************************************************/


int mrmailbox_set_config(mrmailbox_t* ths, const char* key, const char* value)
{
	int ret;

	if( ths == NULL || key == NULL ) { /* "value" may be NULL */
		return 0;
	}

	mrsqlite3_lock(ths->m_sql);
		ret = mrsqlite3_set_config__(ths->m_sql, key, value);
	mrsqlite3_unlock(ths->m_sql);

	return ret;
}


char* mrmailbox_get_config(mrmailbox_t* ths, const char* key, const char* def)
{
	char* ret;

	if( ths == NULL || key == NULL ) { /* "def" may be NULL */
		return safe_strdup(def);
	}

	mrsqlite3_lock(ths->m_sql);
		ret = mrsqlite3_get_config__(ths->m_sql, key, def);
	mrsqlite3_unlock(ths->m_sql);

	return ret; /* the returned string must be free()'d, returns NULL on errors */
}


int mrmailbox_set_config_int(mrmailbox_t* ths, const char* key, int32_t value)
{
	int ret;

	if( ths == NULL || key == NULL ) {
		return 0;
	}

	mrsqlite3_lock(ths->m_sql);
		ret = mrsqlite3_set_config_int__(ths->m_sql, key, value);
	mrsqlite3_unlock(ths->m_sql);

	return ret;
}


int32_t mrmailbox_get_config_int(mrmailbox_t* ths, const char* key, int32_t def)
{
	int32_t ret;

	if( ths == NULL || key == NULL ) {
		return def;
	}

	mrsqlite3_lock(ths->m_sql);
		ret = mrsqlite3_get_config_int__(ths->m_sql, key, def);
	mrsqlite3_unlock(ths->m_sql);

	return ret;
}


char* mrmailbox_get_info(mrmailbox_t* ths)
{
	const char* unset = "0";
	char *displayname = NULL, *info = NULL, *l_readable_str = NULL, *l2_readable_str = NULL, *fingerprint_str = NULL;
	mrloginparam_t *l = NULL, *l2 = NULL;
	int contacts, chats, real_msgs, deaddrop_msgs, is_configured, dbversion, mdns_enabled, e2ee_enabled, prv_key_count, pub_key_count;
	mrkey_t* self_public = mrkey_new();

	if( ths == NULL ) {
		return safe_strdup("ErrBadPtr");
	}

	/* read data (all pointers may be NULL!) */
	l = mrloginparam_new();
	l2 = mrloginparam_new();

	mrsqlite3_lock(ths->m_sql);

		mrloginparam_read__(l, ths->m_sql, "");
		mrloginparam_read__(l2, ths->m_sql, "configured_" /*the trailing underscore is correct*/);

		displayname     = mrsqlite3_get_config__(ths->m_sql, "displayname", NULL);

		chats           = mrmailbox_get_chat_cnt__(ths);
		real_msgs       = mrmailbox_get_real_msg_cnt__(ths);
		deaddrop_msgs   = mrmailbox_get_deaddrop_msg_cnt__(ths);
		contacts        = mrmailbox_get_real_contact_cnt__(ths);

		is_configured   = mrsqlite3_get_config_int__(ths->m_sql, "configured", 0);

		dbversion       = mrsqlite3_get_config_int__(ths->m_sql, "dbversion", 0);

		e2ee_enabled    = mrsqlite3_get_config_int__(ths->m_sql, "e2ee_enabled", MR_E2EE_DEFAULT_ENABLED);

		mdns_enabled    = mrsqlite3_get_config_int__(ths->m_sql, "mdns_enabled", MR_MDNS_DEFAULT_ENABLED);

		sqlite3_stmt* stmt = mrsqlite3_prepare_v2_(ths->m_sql, "SELECT COUNT(*) FROM keypairs;");
		sqlite3_step(stmt);
		prv_key_count = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);

		stmt = mrsqlite3_prepare_v2_(ths->m_sql, "SELECT COUNT(*) FROM acpeerstates;");
		sqlite3_step(stmt);
		pub_key_count = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);

		if( mrkey_load_self_public__(self_public, l2->m_addr, ths->m_sql) ) {
			fingerprint_str = mrkey_render_fingerprint(self_public, ths);
		}
		else {
			fingerprint_str = safe_strdup("<Not yet calculated>");
		}

	mrsqlite3_unlock(ths->m_sql);

	l_readable_str = mrloginparam_get_readable(l);
	l2_readable_str = mrloginparam_get_readable(l2);

	/* create info
	- some keys are display lower case - these can be changed using the `set`-command
	- we do not display the password here; in the cli-utility, you can see it using `get mail_pw`
	- use neutral speach; the Delta Chat Core is not directly related to any front end or end-product
	- contributors: You're welcome to add your names here */
	info = mr_mprintf(
		"Chats: %i\n"
		"Chat messages: %i\n"
		"Messages in mailbox: %i\n"
		"Contacts: %i\n"
		"Database=%s, dbversion=%i, Blobdir=%s\n"
		"\n"
		"displayname=%s\n"
		"configured=%i\n"
		"config0=%s\n"
		"config1=%s\n"
		"mdns_enabled=%i\n"
		"e2ee_enabled=%i\n"
		"E2EE_DEFAULT_ENABLED=%i\n"
		"Private keys=%i, public keys=%i, fingerprint=\n%s\n"
		"\n"
		"Using Delta Chat Core v%i.%i.%i, SQLite %s-ts%i, libEtPan %i.%i, OpenSSL %i.%i.%i%c. Compiled " __DATE__ ", " __TIME__ " for %i bit usage."
		/* In the frontends, additional software hints may follow here. */

		, chats, real_msgs, deaddrop_msgs, contacts
		, ths->m_dbfile? ths->m_dbfile : unset,   dbversion,   ths->m_blobdir? ths->m_blobdir : unset

        , displayname? displayname : unset
		, is_configured
		, l_readable_str, l2_readable_str

		, mdns_enabled

		, e2ee_enabled
		, MR_E2EE_DEFAULT_ENABLED
		, prv_key_count, pub_key_count, fingerprint_str

		, MR_VERSION_MAJOR, MR_VERSION_MINOR, MR_VERSION_REVISION
		, SQLITE_VERSION, sqlite3_threadsafe()   ,  libetpan_get_version_major(), libetpan_get_version_minor()
		, (int)(OPENSSL_VERSION_NUMBER>>28), (int)(OPENSSL_VERSION_NUMBER>>20)&0xFF, (int)(OPENSSL_VERSION_NUMBER>>12)&0xFF, (char)('a'-1+((OPENSSL_VERSION_NUMBER>>4)&0xFF))
		, sizeof(void*)*8

		);

	/* free data */
	mrloginparam_unref(l);
	mrloginparam_unref(l2);
	free(displayname);
	free(l_readable_str);
	free(l2_readable_str);
	free(fingerprint_str);
	mrkey_unref(self_public);
	return info; /* must be freed by the caller */
}


/*******************************************************************************
 * Misc.
 ******************************************************************************/


int mrmailbox_reset_tables(mrmailbox_t* ths, int bits)
{
	mrmailbox_log_info(ths, 0, "Resetting tables (%i)...", bits);

	mrsqlite3_lock(ths->m_sql);

		if( bits & 1 ) {
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM jobs;");
		}

		if( bits & 2 ) {
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM acpeerstates;");
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM keypairs;");
		}

		if( bits & 8 ) {
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM contacts WHERE id>" MR_STRINGIFY(MR_CONTACT_ID_LAST_SPECIAL) ";"); /* the other IDs are reserved - leave these rows to make sure, the IDs are not used by normal contacts*/
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM chats WHERE id>" MR_STRINGIFY(MR_CHAT_ID_LAST_SPECIAL) ";");
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM chats_contacts;");
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM msgs WHERE id>" MR_STRINGIFY(MR_MSG_ID_LAST_SPECIAL) ";");
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM config WHERE keyname LIKE 'imap.%' OR keyname LIKE 'configured%';");
			mrsqlite3_execute__(ths->m_sql, "DELETE FROM leftgrps;");
		}

	mrsqlite3_unlock(ths->m_sql);

	mrmailbox_log_info(ths, 0, "Tables resetted.");

	ths->m_cb(ths, MR_EVENT_MSGS_CHANGED, 0, 0);

	return 1;
}


char* mrmailbox_get_version_str(void)
{
	return mr_mprintf("%i.%i.%i", (int)MR_VERSION_MAJOR, (int)MR_VERSION_MINOR, (int)MR_VERSION_REVISION);
}


void mrmailbox_wake_lock(mrmailbox_t* mailbox)
{
	if( mailbox == NULL ) {
		return;
	}
	pthread_mutex_lock(&mailbox->m_wake_lock_critical);
		mailbox->m_wake_lock++;
		if( mailbox->m_wake_lock == 1 ) {
			mailbox->m_cb(mailbox, MR_EVENT_WAKE_LOCK, 1, 0);
		}
	pthread_mutex_unlock(&mailbox->m_wake_lock_critical);
}


void mrmailbox_wake_unlock(mrmailbox_t* mailbox)
{
	if( mailbox == NULL ) {
		return;
	}
	pthread_mutex_lock(&mailbox->m_wake_lock_critical);
		if( mailbox->m_wake_lock == 1 ) {
			mailbox->m_cb(mailbox, MR_EVENT_WAKE_LOCK, 0, 0);
		}
		mailbox->m_wake_lock--;
	pthread_mutex_unlock(&mailbox->m_wake_lock_critical);
}


/*******************************************************************************
 * Connect
 ******************************************************************************/


void mrmailbox_connect_to_imap(mrmailbox_t* ths, mrjob_t* job /*may be NULL if the function is called directly!*/)
{
	int             is_locked = 0;
	mrloginparam_t* param = mrloginparam_new();

	if( mrimap_is_connected(ths->m_imap) ) {
		mrmailbox_log_info(ths, 0, "Already connected or trying to connect.");
		goto cleanup;
	}

	mrsqlite3_lock(ths->m_sql);
	is_locked = 1;

		if( mrsqlite3_get_config_int__(ths->m_sql, "configured", 0) == 0 ) {
			mrmailbox_log_error(ths, 0, "Not configured.");
			goto cleanup;
		}

		mrloginparam_read__(param, ths->m_sql, "configured_" /*the trailing underscore is correct*/);

	mrsqlite3_unlock(ths->m_sql);
	is_locked = 0;

	if( !mrimap_connect(ths->m_imap, param) ) {
		mrjob_try_again_later(job, MR_STANDARD_DELAY);
		goto cleanup;
	}

cleanup:
	if( param ) {
		mrloginparam_unref(param);
	}

	if( is_locked ) {
		mrsqlite3_unlock(ths->m_sql);
	}
}


void mrmailbox_connect(mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	mrsqlite3_lock(ths->m_sql);

		ths->m_smtp->m_log_connect_errors = 1;
		ths->m_imap->m_log_connect_errors = 1;

		mrjob_kill_action__(ths, MRJ_CONNECT_TO_IMAP);
		mrjob_add__(ths, MRJ_CONNECT_TO_IMAP, 0, NULL);

	mrsqlite3_unlock(ths->m_sql);
}


void mrmailbox_disconnect(mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	mrsqlite3_lock(ths->m_sql);
		mrjob_kill_action__(ths, MRJ_CONNECT_TO_IMAP);
	mrsqlite3_unlock(ths->m_sql);

	mrimap_disconnect(ths->m_imap);
	mrsmtp_disconnect(ths->m_smtp);
}


int mrmailbox_fetch(mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return 0;
	}

	return mrimap_fetch(ths->m_imap);
}


int mrmailbox_restore(mrmailbox_t* ths, time_t seconds_to_restore)
{
	if( ths == NULL ) {
		return 0;
	}

	return mrimap_restore(ths->m_imap, seconds_to_restore);
}


void mrmailbox_heartbeat(mrmailbox_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	//mrmailbox_log_info(ths, 0, "<3 Mailbox");
	mrimap_heartbeat(ths->m_imap);
}

