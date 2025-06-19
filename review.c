#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* CONFIG CONSTANTS */
/* file paths and names */
static const char datadir[] = "/.local/share/ccq/";
static const char db[]      = "db.bin";
static const char sl[]      = "zh";

/* keys that count as 'pass' */
static const char pass_chs[] = {'\n', 'p', 'P', ' '};

/* 'r'andom, 'o'ldest, 'n'ewest */
static const char default_order = 'r';

/* db.bin's header */
static const char magic[] = "CUNCHUQI";

/* DATA STRUCTURES */
typedef struct {
	int     ct, cap;
	time_t *epochs;
	char  **epoch_ptrs, **fsrs_ptrs, **fronts, **backs;
	int    *fr_lens, *bk_lens;
} DueCards;

typedef struct {
	int   found;
	char *line_start;
	off_t offset;
	char *fr_ptr;
	int   fr_len;
	char *bk_ptr;
	int   bk_len;
} SLRes;

typedef struct {
	char     magic[8];
	uint32_t entryct;
	uint32_t str_size;
} DBHeader;

typedef struct {
	uint32_t key_off;
	uint32_t dname_off;
	uint32_t back_off;
} DBRec;

typedef struct {
	const char *key;
	const char *dname;
	const char *back;
} DBMatch;

typedef struct {
	int      ct;
	DBMatch *matches;
	char    *mmap_addr;
	size_t   mmap_len;
} DBRes;

typedef struct {
	const char *key;
	const char *str_base;
} KeyInfo;

/* OTHER CONSTANTS */
/* user interaction messages
 * it's easier to edit them all gathered up here */
static const char choice_prompt[]   = "\n输入编号，或直接回车：";
static const char fail_msg[]        = "\n未通过\n\n";
static const char keyfound_msg[]    = "该词已在学习列表。\n";
static const char keynotfound_msg[] = "词库中未找到该词。\n";
static const char nocards_msg[]     = "暂无可复习的卡片。\n";
static const char num_too_big[]     = "编号超出范围，请重试。\n";
static const char pass_msg[]        = "\n通过\n\n";
static const char start_msg[]       = "存储器… \033[1m开始！\033[22m 待复习：";
static const char exit_msg[]        = "用户退出…\n";
static const char weird_input[]     = "非法输入，请输入数字。\n";

/* field constants */
static const int  epoch_len   = 10;
static const int  fsrs_len    = 12;
static const char fsrs_init[] = "00;0250;0500"; 

/* signal handling */
static volatile sig_atomic_t terminate = 0;
static int term_sig  = 0;

/* fsrs constants */
static const double W[19]={
	0.40255,1.18385,3.17300,15.69105,7.19490,0.53450,1.46040,0.00460,
	1.54575,0.11920,1.01925,1.93950,0.11000,0.29605,2.26980,0.23150,
	2.98980,0.51655,0.66210
};
static const float F = 19.0/81.0;
static const float C = -0.5;

/* FUNCTIONS */
DBRes search_db(const char *key, int klen, const char *dbpath);
SLRes search_sl(const char *slpath, const char *key, int klen, char **out_addr, size_t *out_length);
char *build_ccq_path(const char *suffix);
char  get_instant_char(void);
long  schedule(int g,int reps,double s,double d,double elap,double rd);
int   cmp_keys(const void *key_info, const void *db_rec);
void *err_malloc(const char *varname, size_t size);
void  die(const char *fmt, ...);
void  handle_signal(int sig);
void  query(const char *key, const char *dbpath, const char *slpath);
void  review_list(const char *slpath, char order);
void  usage(void);

/* START */
int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	char  *home, *key;
	char   ch, order, doquery;
	char   slpath[PATH_MAX], dbpath[PATH_MAX];
	struct sigaction sa;

	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);  /* ctrl c */
	sigaction(SIGTERM, &sa, NULL); /* ctrl d */
	sigaction(SIGHUP, &sa, NULL);  /* WM close window */

	order = 0, doquery = 0;
	while ((ch = getopt(argc, argv, "norq:")) != -1) {
		switch (ch) {
			case 'n': 
				order = 'n'; 
				break;
			case 'o': 
				if (order) 
					usage(); 
				order = 'o'; 
				break;
			case 'r': 
				if (order) 
					usage(); 
				order = 'r'; 
				break;
			case 'q': 
				if (order)
					usage();
				doquery = 1; 
				key = optarg; 
				break;
			default: 
				usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc) 
		die("extra arg [%s]", argv[0]);

	home = getenv("HOME");
	if (!home)
		die("home envp not set");
	snprintf(slpath, sizeof(slpath),"%s%s%s", 
			home, datadir, sl);
	snprintf(dbpath, sizeof(dbpath), "%s%s%s", 
			home, datadir, db);

	if (!order)
		order = default_order;

	if (doquery)
		query(key, dbpath, slpath);
	else 
		review_list(slpath, order);

	return 0;
}

/* parses study list for due cards, quizzes user, updates fields */
void
review_list(const char *slpath, const char order)
{
	DueCards dues;
	char    *addr, *cur, *end;
	char    *res_arr;
	char     tmp_char;
	int      fd, revct, nel_pass;
	int     *rev_ord;
	size_t   length;
	struct   stat sb;
	time_t   now;

	
	/* we open the study list, mmap it for read/write and close it */
	fd = open(slpath, O_RDWR);
	if (fd < 0 || fstat(fd, &sb) < 0) 
		die("review_list: open/fstat %s", slpath);
	length = sb.st_size;
	if (!length)
		die("empty sl");
	addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) die("review_list: mmap sl");
	close(fd);

	/* prepare the vars */
	dues.ct = 0;
	dues.cap = 64;
	dues.epochs      = err_malloc("dues.epochs", dues.cap * sizeof(time_t));
	dues.epoch_ptrs  = err_malloc("dues.epoch_ptrs", dues.cap * sizeof(char *));
	dues.fsrs_ptrs   = err_malloc("dues.fsrs_ptrs", dues.cap * sizeof(char *));
	dues.fronts      = err_malloc("dues.fronts", dues.cap * sizeof(char *));
	dues.backs       = err_malloc("dues.backs", dues.cap * sizeof(char *));
	dues.fr_lens     = err_malloc("dues.fr_lens", dues.cap * sizeof(int));
	dues.bk_lens     = err_malloc("dues.bk_lens", dues.cap * sizeof(int));

	now = time(NULL);
	cur = addr;
	end = addr + length;

	/* gather due cards */
	for (;;) {
		char   *line_end, *epoch_p, *fsrs_p, *front_p, *back_p;
		time_t  tmp_epoch;

		if (terminate) 
			die("%s", strsignal(term_sig));
		if (cur >= end) /* EOF */
			break;

		/* stretch struct */
		if (dues.ct >= dues.cap) {
			dues.cap *= 2;
			dues.epochs     = realloc(dues.epochs, dues.cap * sizeof(time_t));
			dues.epoch_ptrs = realloc(dues.epoch_ptrs, dues.cap * sizeof(char *));
			dues.fsrs_ptrs  = realloc(dues.fsrs_ptrs, dues.cap * sizeof(char *));
			dues.fronts     = realloc(dues.fronts, dues.cap * sizeof(char *));
			dues.backs      = realloc(dues.backs, dues.cap * sizeof(char *));
			dues.fr_lens    = realloc(dues.fr_lens, dues.cap * sizeof(int));
			dues.bk_lens    = realloc(dues.bk_lens, dues.cap * sizeof(int));
			if (!dues.epochs || !dues.fsrs_ptrs || !dues.fronts || !dues.backs ||
			    !dues.fr_lens || !dues.bk_lens)
				die("review_list realloc");
		}

		/* get the pointers to determine start/end of:
		 * - line
		 * - epoch field
		 * - front field
		 * - back field 
		 *  given this formatting:
		 *  EPOCH|FSRS|FRONT|BACK\n */

		line_end = memchr(cur, '\n', end - cur);
		if (!line_end) /* EOF */
			line_end = end;
		epoch_p = cur;

		/* first check if due to skip the rest of the logic */
		tmp_char           = epoch_p[epoch_len];
		epoch_p[epoch_len] = '\0';
		tmp_epoch          = (time_t)strtol(epoch_p, NULL, 10);
		epoch_p[epoch_len] = tmp_char;
		if (tmp_epoch > now) 
			goto end_gather_loop_review;

		/* card is due, get rest of fields */
		fsrs_p  = epoch_p + epoch_len + 1;
		front_p = fsrs_p + fsrs_len + 1;
		back_p  = memchr(front_p, '|', line_end - front_p);

		/* populate struct */
		dues.epochs[dues.ct]     = tmp_epoch;
		dues.epoch_ptrs[dues.ct] = epoch_p;
		dues.fsrs_ptrs[dues.ct]  = fsrs_p;
		dues.fronts[dues.ct]     = front_p;
		dues.fr_lens[dues.ct]    = back_p - front_p;
		dues.backs[dues.ct]      = back_p + 1;
		dues.bk_lens[dues.ct]    = line_end - (back_p + 1);

		/* increment and go to next line */
		++dues.ct;
end_gather_loop_review:
		cur = line_end + 1;
		if (line_end == end) 
			break;
	}

	if (dues.ct == 0)
		die(nocards_msg);

	/* review the gathered cards according to flags
	 * 'r'andom, 'o'ldest first or 'n'ewest first */
	rev_ord = err_malloc("rev_ord", dues.ct * sizeof(int));
	/* -o: no change, since study lists are append-only */
	for (int i = 0; i < dues.ct; ++i) 
		rev_ord[i] = i;
	if (order == 'n') {
		/* reverse order */
		for (int i = 0; i < dues.ct / 2; ++i) {
			int tmp                  = rev_ord[i];
			rev_ord[i]               = rev_ord[dues.ct - 1 - i];
			rev_ord[dues.ct - 1 - i] = tmp;
		}
	} else if (order == 'r') {
		/* Fischer-Yates shuffle */
		srand(now);
		for (int i = dues.ct - 1; i > 0; --i) {
			int j      = rand() % (i + 1);
			int tmp    = rev_ord[i];
			rev_ord[i] = rev_ord[j];
			rev_ord[j] = tmp;
		}
	}

	printf("%s%d张\n\n", start_msg, dues.ct);

	revct = 0;
	res_arr = err_malloc("res_arr", dues.ct);
	nel_pass = sizeof(pass_chs) / sizeof(pass_chs[0]);

	/* user quizz loop */
	for (; revct < dues.ct; ++revct) {
		if (terminate) 
			break;
		int k = rev_ord[revct];
		int user_ch;
		int is_passed = 0;

		/* show front, wait for any key */
		printf("[%d/%d]  ", revct + 1, dues.ct);
		printf("%.*s", dues.fr_lens[k], dues.fronts[k]);
		fflush(stdout);
		user_ch = get_instant_char();
		if (user_ch == EOF) { terminate = 1; break; }

		/* show back, store key */
		printf("\n");
		printf("%.*s", dues.bk_lens[k], dues.backs[k]);
		fflush(stdout);
		user_ch = get_instant_char();
		if (user_ch == EOF) { terminate = 1; break; }

		/* match key against passing characters' array */
		for (int i = 0; i < nel_pass; ++i) {
			if (user_ch == pass_chs[i]) {
				is_passed = 1;
				res_arr[k] = 1;
				write(1, pass_msg, sizeof(pass_msg) - 1);
				break;
			}
		}
		if (!is_passed) {
			res_arr[k] = 0;
			write(1, fail_msg, sizeof(fail_msg) - 1);
		}
	}

	if (terminate)
		printf("\n\n[%s]... updating cards and exiting\n", strsignal(term_sig));

	/* reschedule according to stored result */
	for (int i = 0; i < revct; ++i) {
		int k = rev_ord[i];
		int grade, reps, new_reps;
		double stab, diff, elap, nxt;
		time_t new_epoch;
		char new_epoch_str[epoch_len + 1];
		char new_fsrs_str[fsrs_len + 1];

		/* parse fsrs field */
		dues.fsrs_ptrs[k][2] = '\0';
		reps = strtol(dues.fsrs_ptrs[k], NULL, 10);
		dues.fsrs_ptrs[k][2] = ';';
		dues.fsrs_ptrs[k][7] = '\0';
		stab = strtol(dues.fsrs_ptrs[k] + 3, NULL, 10) / 100.0;
		dues.fsrs_ptrs[k][7] = ';';
		diff = strtol(dues.fsrs_ptrs[k] + 8, NULL, 10) / 100.0;

		/* call the scheduler function */
		grade = res_arr[k] ? 3 : 1;
		elap  = (double)(now - dues.epochs[k]) / 86400.0;
		/* we get the interval to add, in seconds */
		nxt   = schedule(grade, reps, stab, diff, elap, 0.9);

		/* derive the new vars */
		new_epoch = now + (time_t)(nxt + 0.5);
		new_reps  = (grade == 1) ? 0 : (reps < 99 ? reps + 1 : 99);
		stab      = (grade == 1) ? 2.50 : stab + stab / 4.0 + 0.25;
		if (stab > 99.99) 
			stab = 99.99;
		diff      = (grade == 1) ? 5.00 : (diff > 1.25 ? diff - 0.25 : 1.00);

		/* overwrite the fixed fields (date and fsrs) */
		snprintf(new_epoch_str, sizeof(new_epoch_str), "%-*ld", (int)epoch_len, new_epoch);
		snprintf(new_fsrs_str, sizeof(new_fsrs_str), "%02d;%04d;%04d",
				new_reps, (int)(stab * 100 + 0.5), (int)(diff * 100 + 0.5));

		memcpy(dues.epoch_ptrs[k], new_epoch_str, epoch_len);
		memcpy(dues.fsrs_ptrs[k],  new_fsrs_str,  fsrs_len);
	}

	/* clean up everything */
	munmap(addr, length);
	free(dues.epochs); 
	free(dues.epoch_ptrs); 
	free(dues.fsrs_ptrs);
	free(dues.fronts); 
	free(dues.backs); 
	free(dues.fr_lens); 
	free(dues.bk_lens);
	free(res_arr);
	free(rev_ord);
	return;
}

/* search for key in study list, return parsed struct or keeps field found = 0 */
SLRes
search_sl(const char *slpath, const char *key, const int klen,
		char **out_addr, size_t *out_length)
{
	SLRes res = {0};
	char *addr, *cur, *end;
	int fd;
	struct stat sb;

	*out_addr = NULL;
	*out_length = 0;

	/* open and mmap study list */
	fd = open(slpath, O_RDONLY);
	if (fd < 0 || fstat(fd, &sb) < 0) 
		die("open sl");
	addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (addr == MAP_FAILED) 
		die("mmap sl");
	close(fd);

	/* derive the mmap's variables */
	*out_addr   = addr;
	*out_length = sb.st_size;
	cur         = addr;
	end         = addr + *out_length;

	/* linearly search file for key */
	while (cur < end) {
		char *line_start, *line_end, *fr_ptr, *bk_ptr;
		int fr_len;

		/* get the start/end of
		 * - line
		 * - front
		 * - back */

		line_start = cur;
		line_end = memchr(cur, '\n', end - cur);
		if (!line_end) /* EOF */
			line_end = end;

		fr_ptr = cur + epoch_len + fsrs_len + 2;
		bk_ptr = memchr(fr_ptr, '|', line_end - fr_ptr);
		fr_len = bk_ptr - fr_ptr;

		/* first check for length (inexpensive)
		 * then compare the keys and store match */
		if (klen == fr_len && memcmp(fr_ptr, key, klen) == 0) {
			res.found      = 1;
			res.line_start = line_start;
			res.offset     = line_start - addr;
			res.fr_ptr     = fr_ptr;
			res.fr_len     = fr_len;
			res.bk_ptr     = bk_ptr + 1;
			res.bk_len     = line_end - (bk_ptr + 1);
			return res;
		}

		/* go to next line */
		cur = line_end + 1;
	}

	/* returns 0: no match */
	return res;
}

/* adds key to sl if found in db and not in sl; user chooses definition */
void
query(const char *key, const char *dbpath, const char *slpath)
{
	int klen;
	char *sl_mmap_addr;
	size_t sl_mmap_len;

	/* simple 'count bytes' strlen, guaranteed to work with wide chars */
	klen = 0;
	while (key[klen])
		++klen;

	/* guard against duplicates */
	SLRes sl_res = search_sl(slpath, key, klen, &sl_mmap_addr, &sl_mmap_len);
	if (sl_res.found) {
		printf("%s\n%.*s\n", keyfound_msg, sl_res.bk_len, sl_res.bk_ptr);
		if (sl_mmap_addr) 
			munmap(sl_mmap_addr, sl_mmap_len);
		return;
	}
	if (sl_mmap_addr) 
		munmap(sl_mmap_addr, sl_mmap_len);

	/* search the database for a match
	 * note that we already exit if no matches in search_db() */
	DBRes db_res = search_db(key, klen, dbpath);

	printf("\t\033[1m%s\033[0m\n", key);
	for (int i = 0; i < db_res.ct; i++) {
		/* print [index] [dictionary name] definition string */
		printf("[%d] ", i);
		printf("[\033[36m%s\033[0m] ", db_res.matches[i].dname);
		printf("%s\n", db_res.matches[i].back);
	}

	/* let user choose index */
	int chosen_idx = -1, choice;
	char input_buf[16];
	char *endptr;

choose_index:
	printf("%s", choice_prompt);
	if (!fgets(input_buf, sizeof(input_buf), stdin)) 
		die("query: read error");
	if (input_buf[0] == '\n')
		die(exit_msg);

	input_buf[strcspn(input_buf, "\n")] = '\0';
	choice = (int)strtol(input_buf, &endptr, 10);

	if (endptr == input_buf || *endptr != '\0') {
		printf("%s", weird_input);
		goto choose_index;
	} else if (choice < 0 || choice >= db_res.ct) {
		printf("%s", num_too_big);
		goto choose_index;
	}

	chosen_idx = choice;

	/* build the string to append */
	DBMatch *match;
	time_t now;
	int bk_len;
	size_t final_len;
	char *final_str;

	match = &db_res.matches[chosen_idx];
	now = time(NULL);
	bk_len = 0;
	while(match->back[bk_len])
		++bk_len;
	final_len = 20 + 1 + fsrs_len + 1 + klen + 1 + bk_len + 2;
	final_str = err_malloc("final_str", final_len);
	snprintf(final_str, final_len, "%ld|%s|%s|%s\n",
		 now, fsrs_init, key, match->back);

	/* write, close, clean up */
	int sl_fd = open(slpath, O_RDWR | O_APPEND | O_CREAT, 0644);
	if (sl_fd < 0) 
		die("query: open slpath");
	write(sl_fd, final_str, final_len);

	close(sl_fd);
	free(final_str);
	free(db_res.matches);
	munmap(db_res.mmap_addr, db_res.mmap_len);
}

/* search for key in db, return array of results or dies */
DBRes
search_db(const char *key, int klen, const char *dbpath)
{
	DBRes res = {0};
	int fd;
	struct stat sb;
	DBHeader *hdr;
	DBRec *idx_table;
	char *pool_base;

	/* open and mmap the database */
	fd = open(dbpath, O_RDONLY);
	if (fd < 0 || fstat(fd, &sb) < 0)
		die("db open");
	res.mmap_len = sb.st_size;
	res.mmap_addr = mmap(NULL, res.mmap_len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (res.mmap_addr == MAP_FAILED) 
		die("mmap db");
	close(fd);
	
	/* the database is structured this way:
	 * 	1. 16 byte header
	 * 		- 8 byte magic string
	 * 		- 4 byte entry count
	 * 		- 4 byte string pool size
	 * 	2. 20*ct byte metadata block
	 * 	3. string pool 
	 */

	/* first check for file integrity (header) */
	hdr = (DBHeader*)res.mmap_addr;
	if (strncmp(hdr->magic, magic, sizeof(hdr->magic))) 
		die("db corrupted");
	if (hdr->entryct == 0)
		die("no entries in db");

	/* skip header to get to metadata */
	idx_table = (DBRec*)(res.mmap_addr + sizeof(DBHeader));

	/* skip metadata to get to string pool */
	pool_base = res.mmap_addr + sizeof(DBHeader) + (hdr->entryct * sizeof(DBRec));

	/* now bsearch the key array for a match */
	KeyInfo q_info = { key, pool_base };
	DBRec *found_rec = bsearch(&q_info, idx_table, hdr->entryct, sizeof(DBRec), cmp_keys);

	if (!found_rec)
		die(keynotfound_msg);

	long first, cur;
	int ct;

	/* temporary assign first to bsearch's match */
	first = found_rec - idx_table;

	/* go backward to first match */
	while (first > 0 &&
	       cmp_keys(&q_info, &idx_table[first - 1]) == 0)
		--first;

	/* go forward to last match */
	cur = first;
	ct = 0;
	while (cur < (long)hdr->entryct &&
			cmp_keys(&q_info, &idx_table[cur]) == 0) {
		++ct;
		++cur;
	}

	/* populate the results array */
	res.ct = ct;
	res.matches = err_malloc("DB_MatchView array", res.ct * sizeof(DBMatch));
	for (int i = 0; i < res.ct; i++) {
		DBRec *rec    = &idx_table[first + i];
		DBMatch *view = &res.matches[i];
		view->key     = pool_base + rec->key_off;
		view->dname   = pool_base + rec->dname_off;
		view->back    = pool_base + rec->back_off;
	}

	return res;
}

/* comparison fucntion for bsearch */
int
cmp_keys(const void *query_key_info_void, const void *db_record_void)
{
	const KeyInfo *q_info  = (const KeyInfo *)query_key_info_void;
	const DBRec *rec       = (const DBRec *)db_record_void;
	const char *db_key_str = q_info->str_base + rec->key_off;
	return strcmp(q_info->key, db_key_str);
}

/* prints error if any, message, rests ernno and exits */
void 
die(const char *fmt, ...) 
{
	va_list args;
	va_start(args, fmt);

	if (errno)
		perror("");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");

	errno = 0;

	va_end(args);
	exit(1);
}

/* malloc with die on null */
void *err_malloc
(const char *var, size_t size) 
{
	void *ptr = malloc(size);
	if (!ptr) {
		fprintf(stderr, "malloc for %s (%zu bytes) failed: \n", var, size);
		perror("");
		exit(1);
	}
	return ptr;
}

/* print usage and exit */
void 
usage(void) 
{
	fprintf(stderr, "usage: ccq -nor <study_list>\n");
	fprintf(stderr, "       ccq -q <key> -d <database> -s <study_list>\n");
	exit(1);
}

/* store signal and exit */
void
handle_signal(int sig)
{
	term_sig = sig;
	terminate = 1;
}

/* get single char from user without echo or return */
char
get_instant_char(void)
{
	if (terminate) 
		return EOF;

	struct termios oldt, newt;
	char ch = 0;
	int n = 0;

	tcgetattr(STDIN_FILENO, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON | ECHO); 
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	n = read(STDIN_FILENO, &ch, 1);
	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

	if (n == 1) {
		return ch;
	} else if (n == -1) {
		if (errno == EINTR && terminate)
			return EOF;
		else 
			die("read user input failed\n");
	} else if (n == 0) { /* EOF on stdin (e.g., Ctrl+D) */
		terminate = 1;
		return EOF;
	}

	/* we should never reach this point */
	return EOF;
}

/* FSRS SCHEDULING CODE. boring! */

/* retrievability */
double retr(double t,double s) { return pow(1.0+F*(t/s),C); }

/* interval (days) for desired retention rd */
double interval(double rd,double s) { return (s/F)*(pow(rd,1.0/C)-1.0); }

/* initial stability & difficulty */
double s0(int g){return W[g-1];}
double d0(int g)
{
	double d=W[4]-exp(W[5]*(g-1))+1.0;
	return fmin(fmax(d,1.0),10.0);
}

/* stability updates */
double
s_succ(double d,double s,double r,int g)
{
	double a=1.0+(11.0-d)*pow(s,-W[9])*(exp(W[10]*(1.0-r))-1.0)*
		((g==2)?W[15]:1.0)*((g==4)?W[16]:1.0)*exp(W[8]);
	return s*a;
}

double
s_fail(double d,double s,double r)
{
	double sf=W[11]*pow(d,-W[12])*(pow(s+1.0,W[13])-1.0)*exp(W[14]*(1.0-r));
	return fmin(sf,s);
}

double stab(double d,double s,double r,int g) { return (g==1)?s_fail(d,s,r):s_succ(d,s,r,g); }

/* difficulty update */
double dd(int g){return -W[6]*((double)g-3.0);}
double dp(double d,int g){return d+dd(g)*((10.0-d)/9.0);}
double
diff(double d,int g)
{
	double dn=W[7]*d0(4)+(1.0-W[7])*dp(d,g);
	return fmin(fmax(dn,1.0),10.0);
}

/* fsrs scheduler: returns seconds interval to add */
long
schedule(int g,int reps,double s,double d,double elap,double rd)
{
	/* g
	 * 	1 = again
	 * 	3 = good
	 * 	we don't use the other indices
	 * reps
	 * 	numbers of times card has been passed; resets on failure
	 * s, d
	 * 	stability and difficulty
	 * elap
	 * 	days since last review
	 * rd
	 * 	desired retention (0.9)
	 */
	if(!reps){s=s0(g);d=d0(g);}
	double r=retr(elap,s);
	s=stab(d,s,r,g);
	d=diff(d,g);
	double next=interval(rd,s);           /* days */
	double sec=next*86400.0;              /* exact */
	double sec_day=ceil(next)*86400.0; /* whole‑day */
	return ( fabs(sec-elap*86400.0) < fabs(sec_day-elap*86400.0) ) ? sec : sec_day;
}

