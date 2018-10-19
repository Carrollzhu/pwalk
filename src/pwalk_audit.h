// This file contains logic for pwalk -audit functionality to be compiled inline with pwalk.c.
// Arguably, it should be properly modularized with distinct .c and a .h files, but the choice
// here is simply to get it out of pwalk.c expediently.

// Basic logic for extracting <path>'s worm state is ...
// python -c 'import isi.fs.domain;import sys;print isi.fs.domain.get_domain_info('<path>')['worm_state'];'
// worm_state: {'committed': 0, 'ctime': 0L, 'retention_date': 0L}
// python -u // unbuffered I/O mode
// python -E // ignore environment vbls

// log_audit_keys() - Secret decoder ring for .audit output files (awk -F, $column numbers) ...

void
log_audit_keys(void)
{
   fprintf(Plog, "AUDIT: Column indexes for .audit output files ...\n");
   fprintf(Plog, "AUDIT:	 1.  lock_domain_type - SmartLock domain type;\n");
   fprintf(Plog, "AUDIT:	   'E' - Enterprise\n");
   fprintf(Plog, "AUDIT:	   'C' - Compliance\n");
   fprintf(Plog, "AUDIT:	   '-' - Neither\n");
   fprintf(Plog, "AUDIT:	 2.  lock_status - SmartLock lock status;\n");
   fprintf(Plog, "AUDIT:	   '-' - Not locked\n");
   fprintf(Plog, "AUDIT:	   'C' - Committed	(READONLY, NON-DELETABLE)\n");
   fprintf(Plog, "AUDIT:	   'c' - Latent Commit	(READONLY, NON-DELETABLE)\n");
   fprintf(Plog, "AUDIT:	   'X' - eXpired	(READONLY, DELETABLE)\n");
   fprintf(Plog, "AUDIT:	   'O' - Override	(READONLY, NON-DELETABLE)\n");
   fprintf(Plog, "AUDIT:	 3.  w_ref_time - Reference time of worm status enquiry\n");
   fprintf(Plog, "AUDIT:	 4.  st_atime\n");
   fprintf(Plog, "AUDIT:	 5.  st_mtime\n");
   fprintf(Plog, "AUDIT:	 6.  st_ctime\n");
   fprintf(Plog, "AUDIT:	 7.  st_birthtime\n");
   fprintf(Plog, "AUDIT:	 8.  w_ctime\n");
   fprintf(Plog, "AUDIT:	 9.  w_retention_date\n");
   fprintf(Plog, "AUDIT:	10.  eff_auto_date - Ephemeral AutoCommit date\n");
   fprintf(Plog, "AUDIT:	11.  eff_retention_type - Basis of effective expiration date;\n");
   fprintf(Plog, "AUDIT:	   '?' - future (no autocommit, no manually-set value, so not yet ascertainable)\n");
   fprintf(Plog, "AUDIT:	   '!' - future (override)\n");
   fprintf(Plog, "AUDIT:	   '<' - past (committed,past)\n");
   fprintf(Plog, "AUDIT:	   '>' - future (committed,future)\n");
   fprintf(Plog, "AUDIT:	   '*' - future (uncommitted,ephemeral); based on future autocommit)\n");
   fprintf(Plog, "AUDIT:	   '=' - forced (uncommitted,tentative); w_retention_date set in WORM state)\n");
   fprintf(Plog, "AUDIT:	   'E' - error unexpected case w/ persisted expiration!\n");
   fprintf(Plog, "AUDIT:	12.  eff_retention_date - Effective expiration date\n");
   fprintf(Plog, "AUDIT:	13.  st_uid\n");
   fprintf(Plog, "AUDIT:	14.  st_size\n");
   fprintf(Plog, "AUDIT:	15.  st_blocks\n");
   fprintf(Plog, "AUDIT:	16.  \"<ifspath>\"\n");
}

// Shell scripting hints for analyzing .audit files ...
//
// Discover likely 'stuck' worm_state.cdate values ...
// cat *.audit | awk -F, '{print $8}' | sort | uniq -c | sort -n | tail
//
// For 'stuck' worm_state.cdate values, determine diversity of uid ...
// cat *.audit | awk -F, '$8 == 1455685665 {print "uid=",$14}' | sort | uniq -c
//
// Create script to do 'touch -at' commands ...
// cat *.audit | awk -F, '$8 == 1455685665 {print "touch -at",$12,$15}'
//
// One can use touch(1) to change a file's w_ctime, but it will also cause the file's
// expiration date to be persisted using the domain's MINIMUM offset value rather than
// its DEFAULT offset value  -- which can be pretty confusing. A SmartLock-aware
// program should be able to innoculate against the autocommit offset lapsing by
// issuing a net-zero-change metadata-write operation just after opening the file,
// such as doing a chown(2) to re-state the file's owner uid, or perhaps a chmod(2) to
// re-state its mode bit settings. Because open(), chown(), and chmod() are all synchronous
// NFS operations, that would have the effecting of pushing the autocommit time comfortably
// ahead in time.

// CAUTION: pwalk_audit_file() must be re-entrant -- so no static locals!

void
pwalk_audit_file(char *ifspath, struct stat *st, unsigned crc_val, int w_id)
{
   // OneFS-native WORM state info ... (new) ...
   int rc, rc_errno;
   time_t w_ref_time;			// Reference time (now) for evaluating ephemerals
   struct worm_state worm = {};		// WORM state
   struct pctl2_get_expattr_args ge;	// Lots of other metadata

   // Python-based WORM state info ... (deprecated) ...
   char py_p;				// Must always be 'P' in Python response
   int py_rc;
   int py_errno;
   int nvals;
   char pycmd[64], pyout[1024], *pnext, *p;

   // File WORM state values (all whole seconds before 8.???) ...
   int w_committed;		// worm.w_committed		++++
   time_t w_ctime;		// worm.w_ctime			++++
   time_t w_retention_date;	// worm.w_retention_date	++++

   // SmartLock DOMAIN values ...
   time_t w_auto_offset;
   time_t w_min_offset;
   time_t w_max_offset;
   time_t w_def_offset;
   time_t w_override_date;

   // Derived a.k.a. "ephemeral" values ...
   long eff_ctime, eff_auto_date, eff_retention_date;	
   int expired;		// boolean
   time_t offset;	// offset that will be applied at time of commit

   // Formatting buffers and formats ...
   char lock_domain_type, lock_status, eff_retention_type;
   char *default_date_strf;
   char w_ref_time_str[64], *w_ref_time_strf;
   char st_atime_str[64], *st_atime_strf;
   char st_mtime_str[64], *st_mtime_strf;
   char st_ctime_str[64], *st_ctime_strf;
   char st_birthtime_str[64], *st_birthtime_strf;
   char w_ctime_str[64], *w_ctime_strf;
   char w_retention_date_str[64], *w_retention_date_strf;
   char eff_ctime_str[64], *eff_ctime_strf;
   char eff_auto_date_str[64], *eff_auto_date_strf;
   char eff_retention_date_str[64], *eff_retention_date_strf;

   // @@@ Skip all directories ...
   if (S_ISDIR(st->st_mode)) return;

   // @@@ Fetch raw WORM info for OneFS LIN @@@

#ifdef OLD_PYTHON_WAY
/*OLD*/   // --- Fragile pipe-based Python request/response logic for fetching WORM metadata ---
/*OLD*/   //
/*OLD*/   // Python is re-started after every 50,000 calls because it was determined that the
/*OLD*/   // particular logic being used caused it to fail after about 100,000 calls -- presumeably
/*OLD*/   // due to heap exhaustion or other such issue. This logic is a 'Software Aging and
/*OLD*/   // Rejuvenation (SAR) strategy that will not be necessary when this logic is re-coded to
/*OLD*/   // use OneFS native C-language APIs.
/*OLD*/
/*OLD*/   if ((WS[w_id]->NPythonCalls % 50000) == 0) {	// Start or re-start Python co-process
/*OLD*/      if (WDAT.PYTHON_PIPE) {
/*OLD*/         fprintf(WDAT.PYTHON_PIPE, "-1\n");	// Signal script to exit()
/*OLD*/         pclose(WDAT.PYTHON_PIPE);
/*OLD*/      }
/*OLD*/      WDAT.PYTHON_PIPE = popen(PYTHON_COMMAND, "r+");
/*OLD*/      if (VERBOSE > 2) { fprintf(WLOG, "@ <START Python>\n"); fflush(WLOG); }
/*OLD*/   }
/*OLD*/   if (WDAT.PYTHON_PIPE == NULL) abend("Cannot start Python co-process!");
/*OLD*/
/*OLD*/// +++++ 	dom_get_info_by_lin(st.st_ino, st.st_snapid, NULL, NULL, &worm));
/*OLD*/// +++++	return worm.w_committed;
/*OLD*/
/*OLD*/   sprintf(pycmd, "%llu", st->st_ino);
/*OLD*/   fprintf(WDAT.PYTHON_PIPE, "%s\n", pycmd);		// Python REQUEST is just inode (LIN)
/*OLD*/   WS[w_id]->NPythonCalls += 1;
/*OLD*/   fgets(pyout, sizeof(pyout), WDAT.PYTHON_PIPE);	// Python RESPONSE
/*OLD*/   if (VERBOSE > 2) { fprintf(WLOG, "@ << %s", pyout); fflush(WLOG); }
/*OLD*/
/*OLD*/   // Parse OneFS WORM info from Python co-process (normally 12 space-delimited columns) ...
/*OLD*/   //        print 'P',     				-> py_p (literal 'P' always 1st value, else die!)
/*OLD*/   //        print 0,     				-> py_rc (0 on successful fetch of WORM data)
/*OLD*/   //        print 0,     				-> py_errno
/*OLD*/   //        print worm_state['committed'],		-> w_committed
/*OLD*/   //        print w_ref_time, 				-> w_ref_time
/*OLD*/   //        print worm_state['ctime'],			-> w_ctime (WORM compliance clock ctime)
/*OLD*/   //        print worm_state['retention_date'],       	-> w_retention_date
/*OLD*/   //        print domain_info['autocommit_offset'],	-> w_auto_offset
/*OLD*/   //        print domain_info['min_offset'],		-> w_min_offset
/*OLD*/   //        print domain_info['max_offset'],		-> w_max_offset
/*OLD*/   //        print domain_info['default_offset'],	-> w_def_offset
/*OLD*/   //        print domain_info['override_retention']	-> w_override_date
/*OLD*/   // NOTE: 18446744073709551614 == 0x7FFFFFFFFFFFFFFF -- which is default max_retention of FOREVER
/*OLD*/   //	sscanf() on OneFS will not parse it as a signed long long, so unsigned is used to avoid overflow
/*OLD*/   py_p = '?';
/*OLD*/   assert (sizeof(time_t) == 8);
/*OLD*/   nvals = sscanf(pyout, "%c %i %i %llu %i %llu %llu %llu %llu %llu %llu %llu",
/*OLD*/      &py_p,
/*OLD*/      &py_rc,
/*OLD*/      &py_errno,
/*OLD*/      &w_ref_time,
/*OLD*/      &w_committed,
/*OLD*/      &w_ctime,
/*OLD*/      &w_retention_date,
/*OLD*/      &w_auto_offset,
/*OLD*/      &w_min_offset,
/*OLD*/      &w_max_offset,
/*OLD*/      &w_def_offset,
/*OLD*/      &w_override_date);
/*OLD*/   if (((nvals != 12) && (nvals < 3)) || py_p != 'P') {	// Should never happen
/*OLD*/      fflush(WLOG);
/*OLD*/      fprintf(WLOG, "@ \"%s\"\n", ifspath);
/*OLD*/      fprintf(WLOG, "@ pycmd: \"%s\"\n", pycmd);
/*OLD*/      fprintf(WLOG, "@ nvals: %d\n", nvals);
/*OLD*/      fprintf(WLOG, "@ pyout: %s", pyout);
/*OLD*/      fflush(WLOG);
/*OLD*/      while (fgets(pyout, sizeof(pyout), WDAT.PYTHON_PIPE))	// Drain Python output
/*OLD*/         fputs(pyout, WLOG);
/*OLD*/      fflush(WLOG);
/*OLD*/      abend("Python symbiont error! Unexpected response format!\n");
/*OLD*/   }
/*OLD*/
/*OLD*/   if (VERBOSE > 2) {
/*OLD*/      fprintf(WLOG, "@ >> %c %d %d %llu %d %llu %llu %llu %llu %llu %llu %llu\n",
/*OLD*/         py_p, py_rc, py_errno,
/*OLD*/         w_ref_time,
/*OLD*/         w_committed, w_ctime, w_retention_date,
/*OLD*/         w_auto_offset, w_min_offset, w_max_offset, w_def_offset, w_override_date);
/*OLD*/      fflush(WLOG);
/*OLD*/   }
/*OLD*/
/*OLD*/   // @@@ No SmartLock info; return! ...
/*OLD*/   if (py_rc) {
/*OLD*/      WS[w_id]->NPythonErrors += 1;
/*OLD*/      fprintf(WLOG, "-,%d,0,0,0,0,0,0,0,0,0,0,\"?\",0,%u,%lld,%lld,\"%s\"\n",
/*OLD*/         py_rc, st->st_uid, st->st_size, st->st_blocks, ifspath);
/*OLD*/      return;
/*OLD*/   }
#else
   // @@@ Get our w_ref_time ...
   assert(localtime_r(&w_ref_time, NULL));
   // @@@ Get WORM data (OneFS syscall) ...
   // int pctl2_get_expattr(int fd, struct pctl2_get_expattr_args *args);
   rc = pctl2_get_expattr(st->st_ino, &ge);
   rc_errno = errno;
   assert (rc == 0);

   if (VERBOSE > 2) {
      fprintf(WLOG, "@ >> %d/%d %llu %d %llu %llu %llu %llu %llu %llu %llu\n",
         rc, rc_errno,
         w_ref_time,
         w_committed, w_ctime, w_retention_date,
         w_auto_offset, w_min_offset, w_max_offset, w_def_offset, w_override_date);
      fflush(WLOG);
   }
#endif

   // @@@ Derive 'effective' and 'ephemeral' values @@@

   // AutoCommit decisions are based on 'effective' ctime ...
   eff_ctime = (w_ctime) ? w_ctime : st->st_ctime;		// f(Compliance v. Enterprise mode)

   // Effective autocommit time is zero when autocommit offset is zero ...
   if (w_auto_offset)						// Autocommit enabled
      eff_auto_date = eff_ctime + w_auto_offset;		// Ephemeral autocommit date
   else								// No autocommit
      eff_auto_date = 0;

   // First, determine effective (possibly hypothetical) expiration date as ...
   // (a) already persisted, or
   // (b) ephemeral (based on eff_auto_date), or
   // (c) TBD (not ascertainable) ... and then
   if (w_committed) {		// Expiration date previously committed
      eff_retention_date = w_retention_date;
   } else if (eff_auto_date) {	// Expiration date is ephemeral, based on hypothetical future autocommit
      if (w_retention_date == 0) offset = w_def_offset;		// use default
      else offset = w_retention_date - eff_auto_date;		// enforce min and max on w_retention_date
      if (offset < w_min_offset) offset = w_min_offset;
      if (offset > w_max_offset) offset = w_max_offset;
      eff_retention_date = eff_auto_date + w_def_offset;
   } else {			// Expiration depends on when commit occurs
      eff_retention_date = 0;					// Expiration date not ascertainable
   }

   // ... then, apply domain w_override_date iff it is a future date
   if ((w_override_date > eff_retention_date) && (w_retention_date > w_ref_time))
      eff_retention_date = w_override_date;

   // Determin 'expired' ephemeral status (includes 'expired just this second') ...
   expired = w_committed && (eff_retention_date <= w_ref_time);

   // Characterize the effective expiration date ...
   if (expired) {
      eff_retention_type = '<';					// past: committed with eff_retention_date in the past
   } else if ((eff_retention_date != w_retention_date) && (eff_retention_date == w_override_date)) {
      eff_retention_type = '!';					// future: expiration is domain OVERRIDE value
   } else if (w_committed) {
      eff_retention_type = '>';					// future: committed with unexpired eff_retention_date
   } else if (eff_retention_date == 0) {				// ... uncommitted from here on ...
      eff_retention_type = '?';					// unknown: expiration is TBD; not ascertainable
   } else if (eff_auto_date && eff_retention_date) {
      eff_retention_type = '*';					// future: ephemeral, based on autocommit
   } else if (w_retention_date) {
      eff_retention_type = '=';					// future: ephemeral, based on forced setting
   } else {
      eff_retention_type = 'E';					// error: unexpected fall-through!
   }

   // @@@ WORM -audit Output Formatting @@@

   // Format ENTERPRISE versus COMPLIANCE mode ...
   lock_domain_type = w_ctime ? 'C' : 'E';

   // Format COMMITTED state (order of tests very important here) ...
   if (w_committed) {
      lock_status = 'C';							// Committed
   } else {
      lock_status = '-';							// Unlocked
      if (eff_auto_date && (eff_auto_date <= w_ref_time)) lock_status = 'c';	// Latent commit
   }
   if (expired) {
      lock_status = 'X';							// eXpired
      if (eff_retention_date <= w_override_date) lock_status = 'O';		// Override
   }

   // Format dates ... with an eye towards future parametrization ...
   // "%G%m%d%H%M.%S" -> YYYYMMDDhhmm.ss (for 'touch -at YYYYMMDDhhmm.ss <file>')
   // "%F %T" -> "YYYY-MM-DD HH:MM:SS"       
   // NULL -> format raw Unix epoch time (w/o TZ adjustment)

   default_date_strf = NULL;			// Default to Unix epoch time
   st_atime_strf = default_date_strf;
   st_mtime_strf = default_date_strf;
   st_ctime_strf = default_date_strf;
   st_birthtime_strf = default_date_strf;
   w_ref_time_strf = default_date_strf;
   w_ctime_strf = default_date_strf;
   w_retention_date_strf = default_date_strf;
   eff_ctime_strf = default_date_strf;
   eff_auto_date_strf = default_date_strf;
   eff_retention_date_strf = default_date_strf;

   pwalk_format_time_t(&(st->st_atime), st_atime_str, sizeof(st_atime_str), st_atime_strf);
   pwalk_format_time_t(&(st->st_mtime), st_mtime_str, sizeof(st_mtime_str), st_mtime_strf);
   pwalk_format_time_t(&(st->st_ctime), st_ctime_str, sizeof(st_ctime_str), st_ctime_strf);
   pwalk_format_time_t(&(st->st_birthtime), st_birthtime_str, sizeof(st_birthtime_str), st_birthtime_strf);
   pwalk_format_time_t(&w_ref_time, w_ref_time_str, sizeof(w_ref_time_str), w_ref_time_strf);
   pwalk_format_time_t(&w_ctime, w_ctime_str, sizeof(w_ctime_str), w_ctime_strf);
   pwalk_format_time_t(&w_retention_date, w_retention_date_str, sizeof(w_retention_date_str), w_retention_date_strf);
   pwalk_format_time_t(&eff_ctime, eff_ctime_str, sizeof(eff_ctime_str), eff_ctime_strf);
   pwalk_format_time_t(&eff_auto_date, eff_auto_date_str, sizeof(eff_auto_date_str), eff_auto_date_strf);
   pwalk_format_time_t(&eff_retention_date, eff_retention_date_str, sizeof(eff_retention_date_str), eff_retention_date_strf);

   // @@@ OUTPUT (audit): NORMAL -audit output line (14 columns) ...
   // fprintf(WLOG, "%c,%c,%s,%s,%s,%s,%s,%s,%s,%c,%s,%u,%lld,%lld,\"%s\"\n",
   //    lock_domain_type, lock_status,
   //    w_ref_time_str, st_atime_str, st_mtime_str, st_ctime_str, eff_ctime_str, st_birthtime_str,
   //    eff_auto_date_str, eff_retention_type, eff_retention_date_str,
   //    st->st_uid, st->st_size, st->st_blocks, ifspath);
   fprintf(WLOG, "%c,%c,%s,%s,%s,%s,%s,%s,%s,%s,%c,%s,%u,%lld,%lld,\"%s\"\n",
      lock_domain_type, lock_status,
      w_ref_time_str, st_atime_str, st_mtime_str, st_ctime_str, st_birthtime_str,
      w_ctime_str, w_retention_date_str,
      eff_auto_date_str, eff_retention_type, eff_retention_date_str,
      st->st_uid, st->st_size, st->st_blocks, ifspath);
}
