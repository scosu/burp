#include "../burp.h"
#include "../alloc.h"
#include "../asfd.h"
#include "../async.h"
#include "../bu.h"
#include "../cmd.h"
#include "../cntr.h"
#include "../handy.h"
#include "../linkhash.h"
#include "../log.h"
#include "../pathcmp.h"
#include "../prepend.h"
#include "../protocol2/blk.h"
#include "../regexp.h"
#include "../slist.h"
#include "../strlist.h"
#include "bu_get.h"
#include "child.h"
#include "compress.h"
#include "manio.h"
#include "protocol1/restore.h"
#include "protocol2/dpth.h"
#include "protocol2/rblk.h"
#include "protocol2/restore.h"
#include "protocol2/restore_spool.h"
#include "sdirs.h"

static enum asl_ret restore_end_func(struct asfd *asfd,
	struct conf **confs, void *param)
{
	if(!strcmp(asfd->rbuf->buf, "restoreend ok"))
		return ASL_END_OK;
	// Old v2 clients send something slightly different.
	if(!strcmp(asfd->rbuf->buf, "restoreend_ok"))
		return ASL_END_OK;
	iobuf_log_unexpected(asfd->rbuf, __func__);
	return ASL_END_ERROR;
}

static int restore_end(struct asfd *asfd, struct conf **confs)
{
	if(asfd->write_str(asfd, CMD_GEN, "restoreend")) return -1;
	return asfd->simple_loop(asfd, confs, NULL, __func__, restore_end_func);
}

static int srestore_matches(struct strlist *s, const char *path)
{
	int r=0;
	if(!s->flag) return 0; // Do not know how to do excludes yet.
	if((r=strncmp_w(path, s->path))) return 0; // no match
	if(!r) return 1; // exact match
	if(*(path+strlen(s->path)+1)=='/')
		return 1; // matched directory contents
	return 0; // no match
}

// Used when restore is initiated from the server.
static int check_srestore(struct conf **confs, const char *path)
{
	struct strlist *l=get_strlist(confs[OPT_INCEXCDIR]);

	// If no includes specified, restore everything.
	if(!l) return 1;

	for(; l; l=l->next)
		if(srestore_matches(l, path))
			return 1;
	return 0;
}

int want_to_restore(int srestore, struct sbuf *sb,
	regex_t *regex, struct conf **cconfs)
{
	return (!srestore || check_srestore(cconfs, sb->path.buf))
	  && (!regex || regex_check(regex, sb->path.buf));
}

static int setup_cntr(struct asfd *asfd, const char *manifest,
        regex_t *regex, int srestore,
        enum action act, char status, struct conf **cconfs)
{
	int ars=0;
	int ret=-1;
	struct fzp *fzp=NULL;
	struct sbuf *sb=NULL;
	struct cntr *cntr=NULL;

	cntr=get_cntr(cconfs);
	if(!cntr) return 0;

// FIX THIS: this is only trying to work for protocol1.
	if(get_protocol(cconfs)!=PROTO_1) return 0;

	if(!(sb=sbuf_alloc(PROTO_1))) goto end;
	if(!(fzp=fzp_gzopen(manifest, "rb")))
	{
		log_and_send(asfd, "could not open manifest");
		goto end;
	}
	while(1)
	{
		if((ars=sbuf_fill_from_file(sb, fzp, NULL, NULL)))
		{
			if(ars<0) goto end;
			// ars==1 means end ok
			break;
		}
		else
		{
			if(want_to_restore(srestore, sb, regex, cconfs))
			{
				cntr_add_phase1(cntr, sb->path.cmd, 0);
				if(sb->endfile.buf)
				  cntr_add_val(cntr,
					CMD_BYTES_ESTIMATED,
					strtoull(sb->endfile.buf,
						NULL, 10), 0);
			}
		}
		sbuf_free_content(sb);
	}
	ret=0;
end:
	sbuf_free(&sb);
	fzp_close(&fzp);
	return ret;
}

static int restore_sbuf(struct asfd *asfd, struct sbuf *sb, struct bu *bu,
	enum action act, struct sdirs *sdirs, enum cntr_status cntr_status,
	struct conf **cconfs, struct sbuf *need_data, const char *manifest,
	struct slist *slist);

// Used when restoring a hard link that we have not restored the destination
// for. Read through the manifest from the beginning and substitute the path
// and data to the new location.
static int hard_link_substitution(struct asfd *asfd,
	struct sbuf *sb, struct f_link *lp,
	struct bu *bu, enum action act, struct sdirs *sdirs,
	enum cntr_status cntr_status, struct conf **cconfs,
	const char *manifest, struct slist *slist)
{
	int ret=-1;
	struct sbuf *need_data=NULL;
	int last_ent_was_dir=0;
	struct sbuf *hb=NULL;
	struct manio *manio=NULL;
	struct blk *blk=NULL;
	int pcmp;
	enum protocol protocol=get_protocol(cconfs);
	struct cntr *cntr=get_cntr(cconfs);

	if(!(manio=manio_open(manifest, "rb", protocol))
	  || !(need_data=sbuf_alloc(protocol))
	  || !(hb=sbuf_alloc(protocol)))
		goto end;

	if(protocol==PROTO_2)
	{
		  if(!(blk=blk_alloc()))
                	goto end;
	}

	while(1)
	{
		switch(manio_read_with_blk(manio,
			hb, need_data->path.buf?blk:NULL, sdirs))
		{
			case 0: break; // Keep going.
			case 1: ret=0; goto end; // Finished OK.
			default: goto end; // Error;
		}

		if(protocol==PROTO_2)
		{
			if(hb->endfile.buf)
			{
				sbuf_free_content(hb);
				continue;
			}
			if(blk->data)
			{
				if(protocol2_extra_restore_stream_bits(asfd,
					blk, slist, act, need_data,
					last_ent_was_dir, cntr)) goto end;
				continue;
			}
			sbuf_free_content(need_data);
		}

		pcmp=pathcmp(lp->name, hb->path.buf);

		if(!pcmp && (sbuf_is_filedata(hb) || sbuf_is_vssdata(hb)))
		{
			// Copy the path from sb to hb.
			free_w(&hb->path.buf);
			if(!(hb->path.buf=strdup_w(sb->path.buf, __func__)))
				goto end;
			// Should now be able to restore the original data
			// to the new location.
			ret=restore_sbuf(asfd, hb, bu, act, sdirs,
			  cntr_status, cconfs, need_data, manifest, slist);
			// May still need to get protocol2 data.
			if(!ret && need_data->path.buf) continue;
			break;
		}

		sbuf_free_content(hb);
		// Break out once we have gone past the entry that we are
		// interested in.
		if(pcmp<0) break;
	}
end:
	blk_free(&blk);
	sbuf_free(&hb);
	manio_close(&manio);
	return ret;
}

static int restore_sbuf(struct asfd *asfd, struct sbuf *sb, struct bu *bu,
	enum action act, struct sdirs *sdirs, enum cntr_status cntr_status,
	struct conf **cconfs, struct sbuf *need_data, const char *manifest,
	struct slist *slist)
{
	//printf("%s: %s\n", act==ACTION_RESTORE?"restore":"verify", sb->path.buf);
	if(write_status(cntr_status, sb->path.buf, get_cntr(cconfs)))
		return -1;

	if(sb->path.cmd==CMD_HARD_LINK)
	{
		struct f_link *lp=NULL;
		struct f_link **bucket=NULL;
		if((lp=linkhash_search(&sb->statp, &bucket)))
		{
			// It is in the list of stuff that is in the manifest,
			// but was skipped on this restore.
			// Need to go through the manifest from the beginning,
			// and substitute in the data to restore to this
			// location.
			return hard_link_substitution(asfd, sb, lp,
				bu, act, sdirs,
				cntr_status, cconfs, manifest, slist);
			// FIX THIS: Would be nice to remember the new link
			// location so that further hard links would link to
			// it instead of doing the hard_link_substitution
			// business over again.
		}
	}

	if(get_protocol(cconfs)==PROTO_1)
	{
		return restore_sbuf_protocol1(asfd, sb, bu,
		  act, sdirs, cntr_status, cconfs);
	}
	else
	{
		return restore_sbuf_protocol2(asfd, sb,
		  act, cntr_status, get_cntr(cconfs), need_data);
	}
}

int restore_ent(struct asfd *asfd,
	struct sbuf **sb,
	struct slist *slist,
	struct bu *bu,
	enum action act,
	struct sdirs *sdirs,
	enum cntr_status cntr_status,
	struct conf **cconfs,
	struct sbuf *need_data,
	int *last_ent_was_dir,
	const char *manifest)
{
	int ret=-1;
	struct sbuf *xb;

	if(!(*sb)->path.buf)
	{
		logp("Got NULL path!\n");
		return -1;
	}

	// Check if we have any directories waiting to be restored.
	while((xb=slist->head))
	{
		if(is_subdir(xb->path.buf, (*sb)->path.buf))
		{
			// We are still in a subdir.
			break;
		}
		else
		{
			// Can now restore xb because nothing else is fiddling
			// in a subdirectory.
			if(restore_sbuf(asfd, xb, bu,
			  act, sdirs, cntr_status, cconfs, need_data, manifest,
			  slist))
				goto end;
			slist->head=xb->next;
			sbuf_free(&xb);
		}
	}

	/* If it is a directory, need to remember it and restore it later, so
	   that the permissions come out right. */
	/* Meta data of directories will also have the stat stuff set to be a
	   directory, so will also come out at the end. */
	/* FIX THIS: for Windows, need to read and remember the blocks that
	   go with the directories. Probably have to do the same for metadata
	   that goes with directories. */
	if(S_ISDIR((*sb)->statp.st_mode)
	  // Hack for metadata for now - just do it straight away.
	  && !sbuf_is_metadata(*sb))
	{
		// Add to the head of the list instead of the tail.
		(*sb)->next=slist->head;
		slist->head=*sb;

		*last_ent_was_dir=1;

		// Allocate a new sb.
		if(!(*sb=sbuf_alloc(get_protocol(cconfs)))) goto end;
	}
	else
	{
		*last_ent_was_dir=0;
		if(restore_sbuf(asfd, *sb, bu,
		  act, sdirs, cntr_status, cconfs, need_data, manifest,
		  slist))
			goto end;
	}
	ret=0;
end:
	return ret;
}

static int restore_remaining_dirs(struct asfd *asfd, struct bu *bu,
	struct slist *slist, enum action act, struct sdirs *sdirs,
	enum cntr_status cntr_status, struct conf **cconfs)
{
	int ret=-1;
	struct sbuf *sb;
	struct sbuf *need_data=NULL;
	if(!(need_data=sbuf_alloc(get_protocol(cconfs)))) goto end;
	// Restore any directories that are left in the list.
	for(sb=slist->head; sb; sb=sb->next)
	{
		if(get_protocol(cconfs)==PROTO_1)
		{
			if(restore_sbuf_protocol1(asfd, sb, bu, act,
				sdirs, cntr_status, cconfs))
					goto end;
		}
		else
		{
			if(restore_sbuf_protocol2(asfd, sb, act,
				cntr_status, get_cntr(cconfs), need_data))
					goto end;
		}
	}
	ret=0;
end:
	sbuf_free(&need_data);
	return ret;
}

static int restore_stream(struct asfd *asfd, struct sdirs *sdirs,
        struct slist *slist, struct bu *bu, const char *manifest,
	regex_t *regex, int srestore, struct conf **cconfs, enum action act,
        enum cntr_status cntr_status)
{
	int ret=-1;
	int last_ent_was_dir=0;
	struct sbuf *sb=NULL;
	struct iobuf *rbuf=asfd->rbuf;
	struct manio *manio=NULL;
	struct blk *blk=NULL;
	struct sbuf *need_data=NULL;
	enum protocol protocol=get_protocol(cconfs);
	struct cntr *cntr=get_cntr(cconfs);
	struct iobuf interrupt;
	iobuf_init(&interrupt);

	if(protocol==PROTO_2)
	{
		static int rs_sent=0;
		if(!(blk=blk_alloc()))
			goto end;
		if(!rs_sent)
		{
			rs_sent=1;
			if(asfd->write_str(asfd,
				CMD_GEN, "restore_stream")
			  || asfd_read_expect(asfd,
				CMD_GEN, "restore_stream_ok"))
					goto end;
		}
	}

	if(!(manio=manio_open(manifest, "rb", protocol))
	  || !(need_data=sbuf_alloc(protocol))
	  || !(sb=sbuf_alloc(protocol)))
		goto end;

	while(1)
	{
		iobuf_free_content(rbuf);
		if(asfd->as->read_quick(asfd->as))
		{
			logp("read quick error\n");
			goto end;
		}
		if(rbuf->buf) switch(rbuf->cmd)
		{
			case CMD_MESSAGE:
			case CMD_WARNING:
			{
				log_recvd(rbuf, cntr, 0);
				continue;
			}
			case CMD_INTERRUPT:
				if(protocol==PROTO_2)
				{
					iobuf_free_content(&interrupt);
					iobuf_move(&interrupt, rbuf);
				}
				// PROTO_1:
				// Client wanted to interrupt the
				// sending of a file. But if we are
				// here, we have already moved on.
				// Ignore.
				continue;
			default:
				iobuf_log_unexpected(rbuf, __func__);
				goto end;
		}

		switch(manio_read_with_blk(manio,
			sb, need_data->path.buf?blk:NULL, sdirs))
		{
			case 0: break; // Keep going.
			case 1: ret=0; goto end; // Finished OK.
			default: goto end; // Error;
		}

		if(protocol==PROTO_2)
		{
			if(sb->endfile.buf)
			{
				if(act==ACTION_RESTORE
				  && asfd->write(asfd, &sb->endfile))
					goto end;
				sbuf_free_content(sb);
				iobuf_free_content(&interrupt);
				continue;
			}
			if(interrupt.buf)
			{
				if(!need_data->path.buf)
				{
					iobuf_free_content(&interrupt);
				}
				else if(!iobuf_pathcmp(&need_data->path,
					&interrupt))
				{
					if(blk->data)
						blk->data=NULL;
					continue;
				}
			}
			if(blk->data)
			{
				if(protocol2_extra_restore_stream_bits(asfd,
					blk, slist, act, need_data,
					last_ent_was_dir, cntr)) goto end;
				continue;
			}
			sbuf_free_content(need_data);
		}

		if(want_to_restore(srestore, sb, regex, cconfs))
		{
			if(restore_ent(asfd, &sb, slist,
				bu, act, sdirs, cntr_status, cconfs,
				need_data, &last_ent_was_dir, manifest))
					goto end;
		}
		else if(sbuf_is_filedata(sb) || sbuf_is_vssdata(sb))
		{
			// Add it to the list of filedata that was not
			// restored.
			struct f_link **bucket=NULL;
			if(!linkhash_search(&sb->statp, &bucket)
			  && linkhash_add(sb->path.buf, &sb->statp, bucket))
				goto end;
		}

		sbuf_free_content(sb);
	}
end:
	blk_free(&blk);
	sbuf_free(&sb);
	sbuf_free(&need_data);
	iobuf_free_content(rbuf);
	iobuf_free_content(&interrupt);
	manio_close(&manio);
	return ret;
}

static int actual_restore(struct asfd *asfd, struct bu *bu,
	const char *manifest, regex_t *regex, int srestore, enum action act,
	struct sdirs *sdirs, enum cntr_status cntr_status, struct conf **cconfs)
{
        int ret=-1;
	int do_restore_stream=1;
        // For out-of-sequence directory restoring so that the
        // timestamps come out right:
        struct slist *slist=NULL;
	struct cntr *cntr=NULL;

	if(linkhash_init()
          || !(slist=slist_alloc()))
                goto end;

	if(get_protocol(cconfs)==PROTO_2)
	{
		if(rblk_init())
			goto end;
		switch(maybe_restore_spool(asfd, manifest, sdirs, bu,
			srestore, regex, cconfs, slist, act, cntr_status))
		{
			case 1: do_restore_stream=0; break;
			case 0: do_restore_stream=1; break;
			default: goto end; // Error;
		}
	}
	if(do_restore_stream && restore_stream(asfd, sdirs, slist,
		bu, manifest, regex,
		srestore, cconfs, act, cntr_status))
			goto end;

	if(restore_remaining_dirs(asfd, bu, slist,
		act, sdirs, cntr_status, cconfs)) goto end;

	if(cconfs) cntr=get_cntr(cconfs);
	cntr_print(cntr, act);
	cntr_stats_to_file(cntr, bu->path, act, cconfs);
end:
        slist_free(&slist);
	linkhash_free();
	rblk_free();
        return ret;
}

static int get_logpaths(struct bu *bu, const char *file,
	char **logpath, char **logpathz)
{
	if(!(*logpath=prepend_s(bu->path, file))
	  || !(*logpathz=prepend(*logpath, ".gz")))
		return -1;
	return 0;
}

static int restore_manifest(struct asfd *asfd, struct bu *bu,
	regex_t *regex, int srestore, enum action act, struct sdirs *sdirs,
	char **dir_for_notify, struct conf **cconfs)
{
	int ret=-1;
	char *manifest=NULL;
	char *logpath=NULL;
	char *logpathz=NULL;
	// For sending status information up to the server.
	enum cntr_status cntr_status=CNTR_STATUS_RESTORING;

	if(act==ACTION_RESTORE) cntr_status=CNTR_STATUS_RESTORING;
	else if(act==ACTION_VERIFY) cntr_status=CNTR_STATUS_VERIFYING;

	if((act==ACTION_RESTORE && get_logpaths(bu, "restorelog",
		&logpath, &logpathz))
	  || (act==ACTION_VERIFY && get_logpaths(bu, "verifylog",
		&logpath, &logpathz))
	  || !(manifest=prepend_s(bu->path,
		get_protocol(cconfs)==PROTO_1?
			"manifest.gz":"manifest")))
	{
		log_and_send_oom(asfd, __func__);
		goto end;
	}

	if(log_fzp_set(logpath, cconfs))
	{
		char msg[256]="";
		snprintf(msg, sizeof(msg),
				"could not open log file: %s", logpath);
		log_and_send(asfd, msg);
		goto end;
	}

	*dir_for_notify=strdup_w(bu->path, __func__);

	log_restore_settings(cconfs, srestore);

	// First, do a pass through the manifest to set up cntr.
	// This is the equivalent of a phase1 scan during backup.

	if(setup_cntr(asfd, manifest,
		regex, srestore, act, cntr_status, cconfs))
			goto end;

	if(get_int(cconfs[OPT_SEND_CLIENT_CNTR])
	  && cntr_send(get_cntr(cconfs)))
		goto end;

	// Now, do the actual restore.
	ret=actual_restore(asfd, bu, manifest,
		  regex, srestore, act, sdirs, cntr_status, cconfs);
end:
	log_fzp_set(NULL, cconfs);
	compress_file(logpath, logpathz, get_int(cconfs[OPT_COMPRESSION]));
	free_w(&manifest);
	free_w(&logpath);
	free_w(&logpathz);
	return ret;
}

int do_restore_server(struct asfd *asfd, struct sdirs *sdirs,
	enum action act, int srestore,
	char **dir_for_notify, struct conf **confs)
{
	int ret=-1;
	uint8_t found=0;
	struct bu *bu=NULL;
	struct bu *bu_list=NULL;
	unsigned long bno=0;
	regex_t *regex=NULL;
	const char *regexstr=get_string(confs[OPT_REGEX]);
	const char *backup=get_string(confs[OPT_BACKUP]);

	logp("in do_restore\n");

	if(regexstr
	  && *regexstr
	  && !(regex=regex_compile(regexstr)))
	{
		char msg[256]="";
		snprintf(msg, sizeof(msg), "unable to compile regex: %s\n",
			regexstr);
		log_and_send(asfd, msg);
		goto end;
	}

	if(bu_get_list(sdirs, &bu_list))
		goto end;

	if(bu_list &&
	   (!backup
	 || !*backup
	 || (!(bno=strtoul(backup, NULL, 10)) && *backup!='a')))
	{
		found=1;
		// No backup specified, do the most recent.
		for(bu=bu_list; bu && bu->next; bu=bu->next) { }
		ret=restore_manifest(asfd, bu, regex, srestore,
				act, sdirs, dir_for_notify, confs);
	}

	if(!found) for(bu=bu_list; bu; bu=bu->next)
	{
		if(!strcmp(bu->timestamp, backup)
		  || bu->bno==bno || (backup && *backup=='a'))
		{
			found=1;
			//logp("got: %s\n", bu->path);
			ret|=restore_manifest(asfd, bu, regex, srestore,
				act, sdirs, dir_for_notify, confs);
			if(backup && *backup=='a')
				continue;
			break;
		}
	}

	bu_list_free(&bu_list);


	if(found)
	{
		// Restore has nearly completed OK.
		ret=restore_end(asfd, confs);
	}
	else
	{
		logp("backup not found\n");
		asfd->write_str(asfd, CMD_ERROR, "backup not found");
		ret=-1;
	}
end:
	regex_free(&regex);
	return ret;
}
