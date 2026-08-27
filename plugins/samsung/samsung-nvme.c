// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>

#include <libnvme.h>

#include <ccan/endian/endian.h>
#include <shared/compiler-attributes-util.h>
#include <shared/fs-util.h>
#include <shared/io-util.h>
#include <shared/progress-util.h>

#include "cleanup.h"
#include "global-ctx.h"
#include "nvme-cmds.h"
#include "nvme-print.h"
#include "plugin.h"

#define SAMSUNG_PLUGIN_VERSION "3.0.15"

#define SAMSUNG_FILE_NAME_SIZE 2048

#define SAMSUNG_GENERAL_FILE_READ_ERROR                     -9
#define SAMSUNG_GENERAL_FILE_WRITE_ERROR                    -10
#define SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR          -11
#define SAMSUNG_GENERAL_PARSE_OR_DEVICE_OPEN_ERROR          -12
#define SAMSUNG_GENERAL_FILE_OPEN_ERROR                     -13
#define SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR             -14
#define SAMSUNG_GENERAL_NO_SUPPORT_FEATURE                  -15
#define SAMSUNG_GENERAL_MCTP_PACKET_SIZE_EXCEED             -16
#define SAMSUNG_GENERAL_NOT_ENOUGH_SPACE_ERROR              -17
#define SAMSUNG_GENERAL_INVALID_VID_ERROR                   -18
#define SAMSUNG_GENERAL_MEM_ALLOC_ERROR                     -19
#define SAMSUNG_GENERAL_RESPONSE_TIMEOUT_ERROR              -20

static bool g_hide_progress;

static const char *samsung_plugin_status_to_string(__s32 status)
{
	const char *str;

	switch (status) {
	case SAMSUNG_GENERAL_FILE_WRITE_ERROR:
		str = "File write error.";
		break;
	case SAMSUNG_GENERAL_FILE_OPEN_ERROR:
		str = "File open error.";
		break;
	case SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR:
		str = "File name generation error.";
		break;
	case SAMSUNG_GENERAL_PARSE_OR_DEVICE_OPEN_ERROR:
		str = "Parsing or Device Open error.";
		break;
	case SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR:
		str = "Invalid parameter error.";
		break;
	case SAMSUNG_GENERAL_NO_SUPPORT_FEATURE:
		str = "Not supported feature.";
		break;
	case SAMSUNG_GENERAL_MCTP_PACKET_SIZE_EXCEED:
		str = "Not supported cmd in MCTP. Packet size > 4KB.";
		break;
	case SAMSUNG_GENERAL_NOT_ENOUGH_SPACE_ERROR:
		str = "Not enough space to execute.";
		break;
	case SAMSUNG_GENERAL_INVALID_VID_ERROR:
		str = "Only Samsung products are supported.";
		break;
	case SAMSUNG_GENERAL_MEM_ALLOC_ERROR:
		str = "Memory allocation error.";
		break;
	case SAMSUNG_GENERAL_RESPONSE_TIMEOUT_ERROR:
		str = "Response timeout error.";
		break;

	default:
		str = "Unknown.";
	}
	return str;
}

static void samsung_print_error(int err)
{
	if (err > 0) {
		if ((err & 0xFF) == NVME_SC_INVALID_FIELD
				|| (err & 0xFF) == NVME_SC_INVALID_LOG_PAGE) {
			fprintf(stderr, "This device is not supported.\n");
		} else {
			fprintf(stderr, "NVMe Status: %s(0x%X)\n",
					libnvme_status_to_string(err, false), err);
		}
	} else if (err < 0)
		fprintf(stderr, "%s(%d)\n", samsung_plugin_status_to_string(err), err);
}

static void samsung_initialize(void)
{
	// Set stdout to unbuffered (always fflush immediately)
	// Must be called before any other printf().
	setvbuf(stdout, NULL, _IONBF, 0);
}

static int samsung_nvme_submit_admin_passthru(
		struct libnvme_transport_handle *hdl, struct libnvme_passthru_cmd *cmd)
{
	return libnvme_exec_admin_passthru(hdl, cmd);
}

static int samsung_nvme_get_log_page(struct libnvme_transport_handle *hdl,
		__u32 nsid, __u8 log_id, __u32 data_len, void *data, __u64 lpo,
		__u8 lsp, __u8 rae, __u32 cdw14)
{
	struct libnvme_passthru_cmd cmd = {
		.opcode   = nvme_admin_get_log_page,
		.nsid     = nsid,
		.addr     = (__u64)(uintptr_t) data,
		.data_len = data_len,
		.cdw10    = ((lsp & 0x7F) << 8) | log_id,
		.cdw12    = lpo & 0xFFFFFFFF,
		.cdw13    = lpo >> 32,
		.cdw14    = cdw14,
	};

	return libnvme_get_log(hdl, &cmd, rae, data_len);
}

#define UNIT_DATA_SIZE_1KB   (1024)
#define UNIT_DATA_SIZE_5KB   (5 * 1024)
#define UNIT_DATA_SIZE_8KB   (8 * 1024)
#define UNIT_DATA_SIZE_16KB  (16 * 1024)
#define UNIT_DATA_SIZE_32KB  (32 * 1024)
#define UNIT_DATA_SIZE_78KB  (78 * 1024)
#define UNIT_DATA_SIZE_94KB  (94 * 1024)
#define UNIT_DATA_SIZE_127KB (127 * 1024)
#define UNIT_DATA_SIZE_128KB (128 * 1024)
#define UNIT_DATA_SIZE_512KB (512 * 1024)

static void get_serial_number(struct nvme_id_ctrl *ctrl, char *sn)
{
	int i;
	// Remove trailing spaces from the name
	for (i = 0; i < sizeof(ctrl->sn); i++) {
		if (ctrl->sn[i] == ' ')
			break;
		sn[i] = ctrl->sn[i];
	}
}

static void get_file_path(char *feature_name, char *file_name, char *sn,
		char *file_path)
{
	if (file_name == 0) {
		snprintf(file_path, SAMSUNG_FILE_NAME_SIZE, "%s_%s.bin",
				sn, feature_name);
	} else {
		snprintf(file_path, SAMSUNG_FILE_NAME_SIZE, "%s%s_%s.bin",
				file_name, sn, feature_name);
	}
}

static void measure_time(struct timeval *begin, bool set_begin)
{
	if (set_begin)
		gettimeofday(begin, NULL);
	else {
		struct timeval end;

		gettimeofday(&end, NULL);
		if (end.tv_usec < begin->tv_usec)
			printf("Time Taken: %ld.%.6lds\n",
					(long)(end.tv_sec - begin->tv_sec - 1),
					(long)(1000000 + end.tv_usec - begin->tv_usec));
		else
			printf("Time Taken: %ld.%.6lds\n",
					(long)(end.tv_sec - begin->tv_sec),
					(long)(end.tv_usec - begin->tv_usec));
	}
}

/*
 * __s64/__u64 are long on LP64 targets such as ppc64le, so cast in the
 * macro to keep the %lld conversions below correct everywhere.
 */
#define SEC(time) ((long long)((time) / 1000000))
#define USEC_3(time) ((long long)((time) % 1000000 / 1000))

static void measure_loop_time(struct timeval *begin, __s64 loop_cnt,
		__s64 total_loop_cnt)
{
	static __s64 time_10;
	static __s64 time_10_per_loop;
	static __s64 time_100;
	static __s64 time_100_per_loop;
	static __s64 time_1000;
	static __s64 time_1000_per_loop;
	struct timeval end;

	/*
	 * Developer timing, on its own axis: this table answers to --verbose
	 * only, never to --hide-progress.
	 */
	if (!nvme_args.verbose)
		return;

	if (loop_cnt == 9) {
		gettimeofday(&end, NULL);
		time_10 = (end.tv_sec * 1000000 + end.tv_usec)
				- (begin->tv_sec * 1000000 + begin->tv_usec);
		time_10_per_loop = time_10 / 10;
	} else if (loop_cnt == 99) {
		gettimeofday(&end, NULL);
		time_100 = (end.tv_sec * 1000000 + end.tv_usec)
				 - (begin->tv_sec * 1000000 + begin->tv_usec);
		time_100_per_loop = time_100 / 100;
	} else if (loop_cnt == 999) {
		gettimeofday(&end, NULL);
		time_1000 = (end.tv_sec * 1000000 + end.tv_usec)
				  - (begin->tv_sec * 1000000 + begin->tv_usec);
		time_1000_per_loop = time_1000 / 1000;
	}

	if (loop_cnt + 1 == total_loop_cnt) {
		printf("Total loops: %lld loops\n", (long long)total_loop_cnt);
		printf("Loops | Elased time | Avg time per loop\n");

		if (total_loop_cnt >= 10) {
			printf("   10 | %7lld.%03lld | %13lld.%03lld\n",
					SEC(time_10), USEC_3(time_10),
					SEC(time_10_per_loop), USEC_3(time_10_per_loop));
		}
		if (total_loop_cnt >= 100) {
			printf("  100 | %7lld.%03lld | %13lld.%03lld\n",
					SEC(time_100), USEC_3(time_100),
					SEC(time_100_per_loop), USEC_3(time_100_per_loop));
		}
		if (total_loop_cnt >= 1000) {
			printf(" 1000 | %7lld.%03lld | %13lld.%03lld\n",
					SEC(time_1000), USEC_3(time_1000),
					SEC(time_1000_per_loop), USEC_3(time_1000_per_loop));
		}

		gettimeofday(&end, NULL);
		__s64 total_time = (end.tv_sec * 1000000 + end.tv_usec)
				- (begin->tv_sec * 1000000 + begin->tv_usec);
		__s64 time_per_loop = total_time / total_loop_cnt;

		printf("%5lld | %7lld.%03lld | %13lld.%03lld\n",
				(long long)total_loop_cnt,
				SEC(total_time), USEC_3(total_time),
				SEC(time_per_loop), USEC_3(time_per_loop));
	}
}

/*
 * Compose result_dir by inserting dir_name as an extra directory level in
 * front of the final component of base_dir, and create that directory:
 *
 *   base_dir      dir_name  result_dir
 *   (NULL)        path2     ./path2/
 *   name          path2     ./path2/name
 *   /path1/       path2     /path1/path2/
 *   /path1/name   path2     /path1/path2/name
 */
static int insert_dir(const char *base_dir, const char *dir_name,
		char *result_dir, size_t result_size)
{
	const char *prefix = "./";
	const char *name = "";
	int prefix_len = 2;
	size_t used;
	int len;
	int ret;

	if (base_dir) {
		name = shr_basename(base_dir);
		if (name != base_dir) {
			prefix = base_dir;
			prefix_len = (int)shr_dir_prefix_len(base_dir);
		}
	}

	len = snprintf(result_dir, result_size, "%.*s%s", prefix_len, prefix,
			dir_name);
	if (len < 0 || (size_t)len >= result_size)
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;
	used = (size_t)len;

	ret = shr_mkdir_p(result_dir, 0777);
	if (ret < 0) {
		fprintf(stderr, "mkdir %s: %s\n", result_dir, strerror(-ret));
		return SAMSUNG_GENERAL_FILE_OPEN_ERROR;
	}

	len = snprintf(result_dir + used, result_size - used, "/%s", name);
	if (len < 0 || (size_t)len >= result_size - used)
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	return 0;
}

/*
 * The archiving below runs through system(), as in the other vendor
 * plugins. Every interpolated argument is single-quoted, so reject the one
 * character that could escape that quoting.
 */
static bool shell_arg_is_safe(const char *s)
{
	return !strchr(s, '\'');
}

static int compress_dump_files(const char *dump_path_with_name, const char *sn)
{
	char tar_cmd[SAMSUNG_FILE_NAME_SIZE * 3] = {0,};
	char rm_cmd[SAMSUNG_FILE_NAME_SIZE * 2] = {0,};
	char targz_file[SAMSUNG_FILE_NAME_SIZE] = {0,};
	const char *dump_name_only = shr_basename(dump_path_with_name);
	int dump_path_len = (int)shr_dir_prefix_len(dump_path_with_name);
	int len;

	if (!shell_arg_is_safe(dump_path_with_name) || !shell_arg_is_safe(sn)) {
		fprintf(stderr, "Output path and serial number must not contain \"'\".\n");
		return SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR;
	}

	len = snprintf(targz_file, sizeof(targz_file),
			"../%sSamsung_Dump_%s.tar.gz", dump_name_only, sn);
	if (len < 0 || (size_t)len >= sizeof(targz_file))
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	len = snprintf(tar_cmd, sizeof(tar_cmd), "cd '%.*s'; tar cvzf '%s' ./*",
			dump_path_len, dump_path_with_name, targz_file);
	if (len < 0 || (size_t)len >= sizeof(tar_cmd))
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	len = snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%.*s'",
			dump_path_len, dump_path_with_name);
	if (len < 0 || (size_t)len >= sizeof(rm_cmd))
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	printf("Compressing...\n");
	if (system(tar_cmd)) {
		fprintf(stderr, "%s failed!\n", tar_cmd);
		return SAMSUNG_GENERAL_FILE_WRITE_ERROR;
	}
	printf("%sSamsung_Dump_%s.tar.gz saved at the designated location.\n",
			dump_name_only, sn);

	if (system(rm_cmd)) {
		fprintf(stderr, "%s failed!\n", rm_cmd);
		return SAMSUNG_GENERAL_FILE_WRITE_ERROR;
	}

	return 0;
}

static void print_border(const char *dump_name, char cmd_type, int arg1, int arg2)
{
	printf("\n-------------------------------------------------------------\n");
	printf("\nExtracting %s", dump_name);
	if (cmd_type == 'g')
		printf("([Get Log] LID %Xh)\n", arg1);
	else if (cmd_type == 's')
		printf("([Secu] SPSP %Xh Op %Xh)\n", arg1, arg2);
	else if (cmd_type == 'v')
		printf("([Vendor] Op %Xh)\n", arg1);
}


/*
 * The HOST/CTLR values double as the Get Log Page LID (0x07 / 0x08), so keep
 * them fixed.  HOST_0 / HOST_1 select the Telemetry Host-Initiated LSP
 * (0 = retain the existing capture, 1 = generate a new one).
 */
enum DUMP_TYPE {
	DUMP_TYPE_ALL  = 0,
	DUMP_TYPE_HOST = 7,
	DUMP_TYPE_CTLR = 8,
	DUMP_TYPE_HOST_0,
	DUMP_TYPE_HOST_1,
	DUMP_TYPE_VENDOR,
	DUMP_TYPE_MAX,
};

#define TELEMETRY_HEADER_SIZE    512
#define TELEMETRY_BYTE_PER_BLOCK 512

#pragma pack(push, 1)

struct reason_identifier {
	__u8 error_id[64];            //[ 63: 0]
	__u8 file_id[8];              //[ 71:64]
	__u8 line_number[2];          //[ 73:72]
	__u8 valid_flags;             //[ 74:74]
	__u8 rsvd75[21];              //[ 95:75]
	__u8 vu_reason_extension[32]; //[127:96]
};

struct telemetry_initiated_log {
	__u8   log_identifier;            //[  0:  0]
	__u8   rsvd1[4];                  //[  4:  1]
	__u8   IEEE[3];                   //[  7:  5]
	__le16 data_area1_last_block;     //[  9:  8]
	__le16 data_area2_last_block;     //[ 11: 10]
	__le16 data_area3_last_block;     //[ 13: 12]
	__u8   rsvd14[2];                 //[ 15: 14]
	__le32 data_area4_last_block;     //[ 19: 16]
	__u8   rsvd20[360];               //[379: 20]
	__u8   _380;                      //[380:380]
	__u8   _381;                      //[381:381]
	__u8   ctlr_init_data_available;  //[382:382]
	__u8   ctlr_init_data_gen_number; //[383:383]
	struct reason_identifier ri;      //[511:384]
};

#pragma pack(pop)

static void print_telemetry_header(struct telemetry_initiated_log *logheader,
		int tele_type)
{
	if (logheader != NULL) {
		unsigned int i = 0, j = 0;

		if (tele_type == DUMP_TYPE_HOST)
			printf("============== Telemetry Host Header ==============\n");
		else
			printf("=========== Telemetry Controller Header ===========\n");

		printf("Log Identifier         : 0x%02X\n", logheader->log_identifier);
		printf("IEEE                   : 0x%02X%02X%02X\n",
				logheader->IEEE[0], logheader->IEEE[1], logheader->IEEE[2]);
		printf("Data Area 1 Last Block : 0x%04X\n",
				le16_to_cpu(logheader->data_area1_last_block));
		printf("Data Area 2 Last Block : 0x%04X\n",
				le16_to_cpu(logheader->data_area2_last_block));
		printf("Data Area 3 Last Block : 0x%04X\n",
				le16_to_cpu(logheader->data_area3_last_block));
		printf("Data Area 4 Last Block : 0x%08X\n",
				le32_to_cpu(logheader->data_area4_last_block));

		if (tele_type == DUMP_TYPE_HOST) {
			printf("Host-init Scope                  : 0x%02X\n",
					logheader->_380);
			printf("Host-init Data Generation Number : 0x%02X\n",
					logheader->_381);
		} else
			printf("Ctlr-init Scope                  : 0x%02X\n",
					logheader->_381);

		printf("Ctlr-init Data Available         : 0x%02X\n",
				logheader->ctlr_init_data_available);
		printf("Ctlr-init Data Generation Number : 0x%02X\n",
				logheader->ctlr_init_data_gen_number);

		printf("\n<Reason Identifier>\n");

		__u8 *ri = (__u8 *)&logheader->ri;

		for (i = 0; i < 8; i++) {
			printf("  ");
			for (j = 0; j < 16; j++)
				printf(" %02X", ri[127 - ((i * 16) + j)]);
			printf("\n");
		}

		printf("===================================================\n");
	}
}

// Get DA4S bit (Data Area 4 Support)
// Admin command - Identify - Identify Controller - Log Page Attributes
static int get_da4s(struct nvme_id_ctrl *ctrl, bool *da4s)
{
	*da4s = ((ctrl->lpa >> 6) & 0x01);
	return 0;
}

// Get MCDAS bit (Maximum Created Data Area Support)
// Admin command - Get Log Page - Supported Log Pages - LID Supported and Effects
static int get_mcdas(struct libnvme_transport_handle *hdl, __u32 nsid, bool *mcdas)
{
	__u8 data[1024] = {0,};
	int err = 0;

	err = samsung_nvme_get_log_page(hdl, nsid, 0, sizeof(data), data, 0, 0, 0, 0);
	if (err != 0) {
		*mcdas = false;
		return err;
	}

	// data[30] is "lower 8 bits of LIDSP" for "LID Supported and Effects Data
	// Structure for LID 7"; data[30] & 1 is MCDAS for LID 7.
	*mcdas = (data[30] & 1) ? true : false;

	return 0;
}

static int get_telemetry_header(struct libnvme_transport_handle *hdl, __u32 nsid,
		__u8 tele_type, __u32 data_len, void *data, __u8 lsp, __u8 rae)
{
	return samsung_nvme_get_log_page(hdl, nsid, tele_type, data_len, data, 0,
			lsp, rae, 0);
}

/*
 * dumpsize is 64-bit: Data Area 4 carries a 32-bit last block number, so a
 * telemetry area can be larger than INT_MAX even though each individual
 * transfer stays within transfersize.
 */
static int extract_telemetry_dump_data(char *featurename, char *file_name,
		char *sn, __s64 dumpsize, int transfersize,
		struct libnvme_transport_handle *hdl, __u32 nsid, __u8 log_id,
		__u8 lsp, __u64 offset, bool rae)
{
	__s64 loop = 0;
	int err = 0;

	const int extra_len = strlen("_header");
	struct timeval begin;
	char *data = calloc(transfersize, sizeof(char));
	char filepath[SAMSUNG_FILE_NAME_SIZE] = {0,};
	char *featurename_header = calloc(SAMSUNG_FILE_NAME_SIZE + extra_len,
			sizeof(char));
	char *filepath_header = calloc(SAMSUNG_FILE_NAME_SIZE + extra_len,
			sizeof(char));
	int output = -1, output_header = -1;
	__s64 total_loop_cnt = dumpsize / transfersize;
	int last_xfer_size = dumpsize % transfersize;
	int retrieve_size;

	if (!data || !featurename_header || !filepath_header) {
		err = SAMSUNG_GENERAL_MEM_ALLOC_ERROR;
		goto end;
	}

	if (last_xfer_size != 0)
		total_loop_cnt++;
	else
		last_xfer_size = transfersize;

	snprintf(featurename_header, SAMSUNG_FILE_NAME_SIZE + extra_len,
			"%s_header", featurename);
	get_file_path(featurename, file_name, sn, filepath);
	get_file_path(featurename_header, file_name, sn, filepath_header);

	measure_time(&begin, true);

	err = get_telemetry_header(hdl, nsid, log_id, TELEMETRY_HEADER_SIZE, data,
			lsp, rae);
	if (err != 0)
		goto end;

	output_header = shr_open_rawdata(filepath_header, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (output_header < 0) {
		err = SAMSUNG_GENERAL_FILE_OPEN_ERROR;
		goto end;
	}

	if (shr_write_all(output_header, data, TELEMETRY_HEADER_SIZE)) {
		err = SAMSUNG_GENERAL_FILE_WRITE_ERROR;
		goto close_output_header;
	}

	for (loop = 0; loop < total_loop_cnt; loop++) {
		retrieve_size = (loop == total_loop_cnt - 1) ? last_xfer_size : transfersize;
		memset(data, 0, transfersize);

		err = samsung_nvme_get_log_page(hdl, nsid, log_id, retrieve_size, data,
				offset, lsp, rae, 0);
		if (err != 0) {
			if (loop > 0)
				goto close_output;
			else
				goto close_output_header;
		}

		if (loop == 0) {
			output = shr_open_rawdata(filepath,
					O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (output < 0) {
				err = SAMSUNG_GENERAL_FILE_OPEN_ERROR;
				goto close_output_header;
			}
		}

		if (shr_write_all(output, data, retrieve_size)
				|| shr_write_all(output_header, data, retrieve_size)) {
			err = SAMSUNG_GENERAL_FILE_WRITE_ERROR;
			goto close_output;
		}

		measure_loop_time(&begin, loop, total_loop_cnt);
		offset += retrieve_size;
		if (!g_hide_progress)
			shr_spinner(featurename,
					(loop + 1) / (float)total_loop_cnt, stdout);
	}

	/* Completion line: printed even under --hide-progress, see above. */
	shr_spinner(featurename, 1.0, stdout);
	printf("\n");
	printf("The log file was saved in \"%s\"\n", filepath);
	printf("The log file was saved in \"%s\"\n", filepath_header);
	measure_time(&begin, false);

close_output:
	if (output >= 0)
		close(output);

close_output_header:
	if (output_header >= 0)
		close(output_header);

end:
	free(filepath_header);
	free(featurename_header);
	free(data);
	return err;
}

static int open_write_close(char *file_name, __u8 *buffer, int size)
{
	int err = 0;
	int output = 0;

	output = shr_open_rawdata(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (output < 0) {
		err = SAMSUNG_GENERAL_FILE_OPEN_ERROR;
		return err;
	}

	if (shr_write_all(output, buffer, size))
		err = SAMSUNG_GENERAL_FILE_WRITE_ERROR;

	close(output);
	return err;
}

static int get_telemetry_dump(struct libnvme_transport_handle *hdl,
		struct nvme_id_ctrl *ctrl, char *file_name, char *sn,
		enum DUMP_TYPE tele_type, int data_area, bool header_print,
		int transfersize)
{
	__u32 err = 0, nsid = NVME_NSID_ALL;
	__u8 lsp = 0, rae = 1;
	char *featurename = 0;

	bool is_enabled_data_area4 = false;
	bool da4s = false;
	bool mcdas = false;
	bool host_behavior_changed = false;

	if (tele_type == DUMP_TYPE_HOST_0) {
		featurename = "Host(0)";
		lsp = 0;
		tele_type = DUMP_TYPE_HOST;
	} else if (tele_type == DUMP_TYPE_HOST_1) {
		featurename = "Host(1)";
		lsp = 1;
		tele_type = DUMP_TYPE_HOST;
	} else { // DUMP_TYPE_CTLR
		featurename = "Controller";
		lsp = 0;
	}

	struct telemetry_initiated_log logheader;
	char dumpname[SAMSUNG_FILE_NAME_SIZE] = {0,};
	char filepath[SAMSUNG_FILE_NAME_SIZE] = {0,};

	// [Data Area 4] Initialize
	if ((data_area == 0) || (data_area == 4))
		is_enabled_data_area4 = true;

	// [Data Area 4] 1. Check DA4S bit (Data Area 4 Support)
	if (is_enabled_data_area4) {
		get_da4s(ctrl, &da4s);
		if (da4s != true) {
			/* Continue on if this fails, it's not a fatal condition */
			printf("\nData Area 4 Support(DA4S) bit is 0.\n");
			is_enabled_data_area4 = false;
		}
	}

	// [Data Area 4] 2. Check MCDAS bit and set MCDA only if creating host log
	if (is_enabled_data_area4 && (lsp == 1)) {
		err = get_mcdas(hdl, nsid, &mcdas);
		if (err) {
			/* Extract Data Area 4 regardless of MCDAS */
			printf("\nGet Maximum Created Data Area Support(MCDAS) "
					"bit is not supported. (0x%X)\n", err);
		} else if (mcdas != true) {
			/* Extract Data Area 4 regardless of MCDAS */
			printf("\nMaximum Created Data Area Support(MCDAS) bit is 0.\n");
		} else {
			// Set MCDA to 4 (Maximum Created Data Area)
			lsp |= (4 << 1);
		}
	}

	// [Data Area 4] 3. Set ETDAS bit (Extended Telemetry Data Area 4 Supported)
	if (is_enabled_data_area4) {
		err = libnvme_set_etdas(hdl, &host_behavior_changed);
		if (err) {
			/* Continue on if this fails, it's not a fatal condition */
			printf("\nSet ETDAS bit is not supported. (0x%X)\n", err);
			is_enabled_data_area4 = false;
		}
	}

	// [Data Area 4] Notify the user whether area 4 can be extracted
	if ((data_area == 0) || (data_area == 4)) {
		if (is_enabled_data_area4 != true) {
			printf("\nData Area 4 will not be extracted. "
					"(It might be displayed as empty.)\n");
			printf("To verify detailed information, please check "
					"the above logs to confirm\n");
			printf("that DA4S, MCDAS, and other settings are "
					"configured correctly.\n\n");
		} else
			printf("\nSetup for Data Area 4 extraction is done.\n\n");
	}

	// Get Header
	err = get_telemetry_header(hdl, nsid, tele_type, TELEMETRY_HEADER_SIZE,
			(void *)&logheader, lsp, rae);
	if (err)
		goto restore_etdas;

	if (header_print)
		print_telemetry_header(&logheader, tele_type);

	snprintf(dumpname, SAMSUNG_FILE_NAME_SIZE, "Telemetry_%s_header_only", featurename);
	get_file_path(dumpname, file_name, sn, filepath);
	err = open_write_close(filepath, (__u8 *)&logheader, TELEMETRY_HEADER_SIZE);
	if (err)
		goto restore_etdas;

	__s64 last_block[5] = {0,};
	__u64 offset[5] = {0,};
	__s64 size[5] = {TELEMETRY_HEADER_SIZE, 0, 0, 0, 0};

	last_block[1] = le16_to_cpu(logheader.data_area1_last_block);
	last_block[2] = le16_to_cpu(logheader.data_area2_last_block);
	last_block[3] = le16_to_cpu(logheader.data_area3_last_block);
	last_block[4] = le32_to_cpu(logheader.data_area4_last_block);

	for (int i = 1; i <= 4; i++) {
		offset[i] = (last_block[i - 1] + 1) * TELEMETRY_HEADER_SIZE;
		size[i] = (last_block[i] - last_block[i - 1]) * TELEMETRY_BYTE_PER_BLOCK;
	}

	int area_start, area_end;

	if (data_area == 0) // extract all area if not designated
		area_start = 1, area_end = 4;
	else // extract designated area only
		area_start = area_end = data_area;

	for (int area = area_start; area <= area_end && err == 0; area++) {
		printf("\n");
		if (size[area] <= 0) {
			printf("Telemetry %s Area %d is empty.\n", featurename, area);
			continue;
		}

		snprintf(dumpname, SAMSUNG_FILE_NAME_SIZE, "Telemetry_%s_Area_%d",
				featurename, area);
		err = extract_telemetry_dump_data(dumpname, file_name, sn, size[area],
				transfersize, hdl, nsid, tele_type, 0, offset[area], rae);
	}

restore_etdas:
	// [Data Area 4] 4. Restore ETDAS bit (Extended Telemetry Data Area 4 Supported)
	if (host_behavior_changed) {
		host_behavior_changed = false;

		int restore_err = libnvme_clear_etdas(hdl, &host_behavior_changed);

		if (restore_err) {
			/* Continue on if this fails, it's not a fatal condition */
			printf("\nRestore ETDAS bit is not supported. (0x%X)\n", restore_err);
		}
	}

	return err;
}

static void print_telemetry_info(enum DUMP_TYPE tele_type, int data_area,
		int lid, int transfer_size)
{
	printf("\n-------------------------------------------------------------\n");

	if (tele_type == DUMP_TYPE_HOST_0)
		printf("\nExtracting Telemetry Host 0 Dump ");
	else if (tele_type == DUMP_TYPE_HOST_1)
		printf("\nExtracting Telemetry Host 1 Dump ");
	else
		printf("\nExtracting Telemetry Controller Dump ");

	if (data_area == 0)
		printf("(Data Area 1 to 4)");
	else
		printf("(Data Area %d)", data_area);

	printf("([Get Log] LID %Xh)", lid);
	printf("(Xfer %dK)\n", transfer_size / 1024);
}

#define MERGE_MAX_AREA 4
#define MERGE_COPY_CHUNK (64 * 1024)

static bool file_is_readable(const char *path)
{
	return !access(path, R_OK);
}

/*
 * Append the whole of path to the already open descriptor out.
 * Return: 0 on success, -errno otherwise.
 */
static int append_file(int out, const char *path)
{
	char *buf;
	int in;
	int ret = 0;

	in = shr_open_rawdata(path, O_RDONLY);
	if (in < 0)
		return -errno;

	buf = malloc(MERGE_COPY_CHUNK);
	if (!buf) {
		close(in);
		return -ENOMEM;
	}

	for (;;) {
		ssize_t n = read(in, buf, MERGE_COPY_CHUNK);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			break;
		}
		if (n == 0)
			break;

		ret = shr_write_all(out, buf, (size_t)n);
		if (ret)
			break;
	}

	free(buf);
	close(in);
	return ret;
}

/*
 * Concatenate the telemetry header and the data area dumps into a single
 * file.
 *
 * get_telemetry_dump() writes no file for an area the controller reports as
 * empty, so the merge list is built from the areas that are actually on
 * disk and the output is named after those alone. Doing this in process
 * rather than through "cat a b c > out" matters: the shell truncates -- and
 * so creates -- the output before cat runs, which left a 0-byte file when
 * no area had been retrieved, and otherwise a name promising areas the file
 * did not contain.
 */
static int merge_telemetry_log(const char *dump_save_dir, const char *sn,
		enum DUMP_TYPE tele_type, int total_area_num)
{
	char area_file[MERGE_MAX_AREA + 1][SAMSUNG_FILE_NAME_SIZE];
	char output_file[SAMSUNG_FILE_NAME_SIZE];
	int found[MERGE_MAX_AREA];
	int found_cnt = 0;
	const char *dump_type;
	size_t used;
	int out;
	int len;
	int err;

	if (tele_type == DUMP_TYPE_HOST_0)
		dump_type = "Host(0)";
	else if (tele_type == DUMP_TYPE_HOST_1)
		dump_type = "Host(1)";
	else if (tele_type == DUMP_TYPE_CTLR)
		dump_type = "Controller";
	else
		return 0;

	if (total_area_num < 1 || total_area_num > MERGE_MAX_AREA)
		return SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR;

	len = snprintf(area_file[0], sizeof(area_file[0]),
			"%s%s_Telemetry_%s_header_only.bin",
			dump_save_dir, sn, dump_type);
	if (len < 0 || (size_t)len >= sizeof(area_file[0]))
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	for (int i = 1; i <= total_area_num; i++) {
		len = snprintf(area_file[i], sizeof(area_file[i]),
				"%s%s_Telemetry_%s_Area_%d.bin",
				dump_save_dir, sn, dump_type, i);
		if (len < 0 || (size_t)len >= sizeof(area_file[i]))
			return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

		if (file_is_readable(area_file[i]))
			found[found_cnt++] = i;
	}

	/*
	 * An area the controller reported as empty is never written, so
	 * finding none means there is simply nothing to merge. A missing
	 * header is a different matter and is left to fail below: it is
	 * written before any area is, and a failure there aborts the dump
	 * before the merge is ever reached.
	 */
	if (!found_cnt) {
		printf("Nothing to merge: no data area was retrieved.\n");
		return 0;
	}

	/* Name the merged dump after the areas it really holds. */
	len = snprintf(output_file, sizeof(output_file),
			"%s%s_Telemetry_%s_Area_%d",
			dump_save_dir, sn, dump_type, found[0]);
	if (len < 0 || (size_t)len >= sizeof(output_file))
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;
	used = (size_t)len;

	for (int i = 1; i < found_cnt; i++) {
		len = snprintf(output_file + used, sizeof(output_file) - used,
				"+%d", found[i]);
		if (len < 0 || (size_t)len >= sizeof(output_file) - used)
			return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;
		used += (size_t)len;
	}

	len = snprintf(output_file + used, sizeof(output_file) - used, ".bin");
	if (len < 0 || (size_t)len >= sizeof(output_file) - used)
		return SAMSUNG_GENERAL_FILE_NAME_GENERATION_ERROR;

	out = shr_open_rawdata(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0)
		return SAMSUNG_GENERAL_FILE_OPEN_ERROR;

	err = 0;
	for (int i = 0; i <= found_cnt && !err; i++) {
		/* Index 0 is the header, which always leads the merge. */
		const char *src = i ? area_file[found[i - 1]] : area_file[0];

		err = append_file(out, src);
		if (err)
			fprintf(stderr, "%s: %s\n", src, strerror(-err));
	}

	close(out);

	if (err)
		return SAMSUNG_GENERAL_FILE_WRITE_ERROR;

	printf("The log file was saved in \"%s\"\n", output_file);
	return 0;
}

/*
 * Retrieve one telemetry dump, reporting a failure as it happens.
 * Collection is best effort, so the status is returned for the sake of
 * try_get_telemetry_all(), which must not merge over a failed retrieval;
 * callers that only collect ignore it.
 */
static int try_get_telemetry(struct libnvme_transport_handle *hdl,
		struct nvme_id_ctrl *ctrl, char *dump_save_dir, char *sn,
		enum DUMP_TYPE tele_type, int data_area, bool header_print)
{
	int err = 0;
	int getlog_lid;
	int tele_xfer_size = UNIT_DATA_SIZE_127KB;

	if (tele_type == DUMP_TYPE_CTLR)
		getlog_lid = 0x08;
	else // Host-init
		getlog_lid = 0x07;

	print_telemetry_info(tele_type, data_area, getlog_lid, tele_xfer_size);

	err = get_telemetry_dump(hdl, ctrl, dump_save_dir, sn, tele_type,
			data_area, header_print, tele_xfer_size);
	if (err != 0)
		samsung_print_error(err);

	return err;
}

/*
 * Retrieve every data area of one telemetry type and merge the results.
 * A host dump is merged twice, into the 1+2 and the 1+2+3+4 view; the
 * controller dump only into the full one.
 */
static void try_get_telemetry_all(struct libnvme_transport_handle *hdl,
		struct nvme_id_ctrl *ctrl, char *dump_save_dir, char *sn,
		enum DUMP_TYPE tele_type)
{
	static const int host_merges[] = {2, 4};
	static const int ctlr_merges[] = {4};
	bool is_ctlr = tele_type == DUMP_TYPE_CTLR;
	const int *merges = is_ctlr ? ctlr_merges : host_merges;
	int nr_merges = is_ctlr ? 1 : 2;

	/*
	 * Nothing to merge if the retrieval failed, and merging anyway would
	 * be worse than nothing: the area files of an earlier run may still
	 * be on disk, and joining those would present stale data as a
	 * complete dump.
	 */
	if (try_get_telemetry(hdl, ctrl, dump_save_dir, sn, tele_type, 0, true))
		return;

	for (int i = 0; i < nr_merges; i++) {
		int err;

		printf("\n-------------------------------------------------------------\n\n");

		err = merge_telemetry_log(dump_save_dir, sn, tele_type, merges[i]);
		if (err != 0)
			samsung_print_error(err);
	}
}

static int samsung_nvme_get_vendor_dump(struct libnvme_transport_handle *hdl,
		__u8 op_code, void *data, __u32 data_len, __u8 sub_op_code,
		__u8 op_type, __u32 offset)
{
	struct libnvme_passthru_cmd cmd = {
		.opcode		= op_code,
		.addr		= (__u64)(uintptr_t) data,
		.data_len	= data_len,
		.cdw10		= data_len / 4,
		.cdw12		= (sub_op_code << 24) | op_type,
		.cdw14		= offset,
		.timeout_ms = 15 * 1000,
	};

	return samsung_nvme_submit_admin_passthru(hdl, &cmd);
}

static int get_vendor_dump_header(struct libnvme_transport_handle *hdl,
		__u8 op_code, __u8 op_type, __u8 *data, int *total_dump_size)
{
	int err = 0;

	err = samsung_nvme_get_vendor_dump(hdl, op_code, data,
			UNIT_DATA_SIZE_8KB, 8, op_type, 0);
	if (err != 0)
		return err;

	*total_dump_size = (data[15] << 24)	| (data[14] << 16)
					 | (data[13] <<  8) |  data[12];

	return err;
}

static int get_vendor_dump_data(struct libnvme_transport_handle *hdl,
		__u8 op_code, __u8 op_type, __u8 *data, __u32 offset)
{
	return samsung_nvme_get_vendor_dump(hdl, op_code, data,
			UNIT_DATA_SIZE_32KB, 8, op_type, offset);
}

static int get_vendor_dump(struct libnvme_transport_handle *hdl,
		char *feature_name, char *file_name, char *sn,
		__u8 op_code, __u8 op_type)
{
	int err = 0;
	int output = 0;
	int offset = 0, total_dump_size = 0;
	char file_path[SAMSUNG_FILE_NAME_SIZE] = {0,};
	struct timeval begin;
	__u8 *data = libnvme_alloc(UNIT_DATA_SIZE_32KB);

	if (!data)
		return SAMSUNG_GENERAL_MEM_ALLOC_ERROR;

	get_file_path(feature_name, file_name, sn, file_path);

	measure_time(&begin, true);

	err = get_vendor_dump_header(hdl, op_code, op_type, data, &total_dump_size);
	if (err != 0)
		goto end;

	output = shr_open_rawdata(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (output < 0) {
		err = SAMSUNG_GENERAL_FILE_OPEN_ERROR;
		goto end;
	}

	if (shr_write_all(output, data, UNIT_DATA_SIZE_8KB)) {
		err = SAMSUNG_GENERAL_FILE_WRITE_ERROR;
		goto close_output;
	}

	for (offset = 1; offset <= total_dump_size; offset += 4) {
		memset(data, 0, UNIT_DATA_SIZE_32KB);
		err = get_vendor_dump_data(hdl, op_code, op_type, data, offset);
		if (err < 0) // last piece returns 2 on some devices.
			goto close_output;

		if (shr_write_all(output, data, UNIT_DATA_SIZE_32KB)) {
			err = SAMSUNG_GENERAL_FILE_WRITE_ERROR;
			goto close_output;
		}

		measure_loop_time(&begin, offset / 4, total_dump_size / 4);
		if (!g_hide_progress)
			shr_spinner(feature_name,
					offset / (float)total_dump_size, stdout);
	}
	/*
	 * The completion line prints even under --hide-progress: it is what
	 * marks the dump as complete in a captured terminal log.
	 */
	shr_spinner(feature_name, 1.0, stdout);
	printf("\n");
	printf("The log file was saved in \"%s\"\n", file_path);
	measure_time(&begin, false);
	err = 0;

close_output:
	close(output);

end:
	libnvme_free(data);
	return err;
}

static int get_vendor_crash_dump_0xf7(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorCrashDump_0xF7", file_name, sn, 0xF7, 0);
}

static int get_vendor_memory_dump_0xf7(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorMemoryDump_0xF7", file_name, sn, 0xF7, 1);
}

static int get_vendor_debug_dump_0xf7(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorDebugDump_0xF7", file_name, sn, 0xF7, 2);
}

static int get_vendor_crash_dump_0xf8(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorCrashDump_0xF8", file_name, sn, 0xF8, 0);
}

static int get_vendor_memory_dump_0xf8(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorMemoryDump_0xF8", file_name, sn, 0xF8, 1);
}

static int get_vendor_debug_dump_0xf8(struct libnvme_transport_handle *hdl,
		char *file_name, char *sn)
{
	return get_vendor_dump(hdl, "VendorDebugDump_0xF8", file_name, sn, 0xF8, 2);
}

/*
 * Run one vendor dump. A status of 1 means the device does not implement
 * that particular dump, which is not a failure of the command.
 */
static void run_vendor_dump(int (*get_dump)(struct libnvme_transport_handle *,
			char *, char *),
		struct libnvme_transport_handle *hdl, const char *label,
		int op_code, char *dump_save_dir, char *sn)
{
	int err;

	print_border(label, 'v', op_code, 0);

	err = get_dump(hdl, dump_save_dir, sn);
	if (err == 1)
		fprintf(stderr, "This device is not supported\n");
	else if (err != 0)
		samsung_print_error(err);
}

/*
 * Translate one dump-type token (e.g. "host0") into a selection in @sel.
 * Called once per comma-separated token so -t accepts a list such as
 * "host0,ctlr,vendor". Returns 0 on success, or a SAMSUNG_GENERAL_* error
 * code if the token is not a recognized dump type.
 */
static int vs_internal_log_select_type(const char *tok, bool *sel)
{
	if (!strcmp(tok, "host0"))
		sel[DUMP_TYPE_HOST_0] = true;
	else if (!strcmp(tok, "host1"))
		sel[DUMP_TYPE_HOST_1] = true;
	else if (!strcmp(tok, "ctlr") || !strcmp(tok, "controller"))
		sel[DUMP_TYPE_CTLR] = true;
	else if (!strcmp(tok, "vendor"))
		sel[DUMP_TYPE_VENDOR] = true;
	else {
		printf("Invalid dump type: '%s'.\n", tok);
		return SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR;
	}

	return 0;
}

/*
 * A dump type is extracted when either no -t was given (select_all, i.e.
 * extract everything) or it was explicitly named in sel[].
 */
#define WANT_DUMP(t) (select_all || sel[(t)])

static int vs_internal_log(int argc, char **argv, struct command *acmd,
		struct plugin *plugin)
{
	const char *desc = "Retrieve and save internal firmware log.";
	const char *type = "Use this option if you want to extract a specific kind of dump.\n"
			"Comma-separate to extract several at once (no spaces).\n"
			"Example) -t host1   or   -t host1,ctlr,vendor\n"
			"host0---Host_Initiated_Telemetry_Dump(LSP_0)\n"
			"host1---Host_Initiated_Telemetry_Dump(LSP_1)\n"
			"ctlr----Controller_Initiated_Telemetry_Dump\n"
			"vendor--Vendor_Dumps\n";
	const char *area = "Telemetry Data Area; 1 to 4. Default: 0(all).";
	const char *file = "Output file name with path;\n"
			"e.g. '-O ./path/name'\n'-O ./path1/path2/';\n"
			"If requested path does not exist, "
			"the directory will be newly created.";
	const char *compress = "Save dumps in a compressed file.";
	const char *hide_progress = "Hide the incremental extraction progress.\n"
			"The completion line of each dump is still printed.";

	int err = 0;
	char sn[21] = {0,};
	struct nvme_id_ctrl ctrl = {0,};
	struct libnvme_passthru_cmd cmd;
	char dump_save_dir[SAMSUNG_FILE_NAME_SIZE] = {0,};

	__cleanup_nvme_global_ctx struct libnvme_global_ctx *ctx = NULL;
	__cleanup_nvme_transport_handle struct libnvme_transport_handle *hdl = NULL;

	bool sel[DUMP_TYPE_MAX] = { false };
	bool select_all = false;
	int tele_data_area = 0;

	struct config {
		char *type;
		int area;
		char *file;
		int compress;
		int hide_progress;
	};

	struct config cfg = {
		.type = NULL,
		.area = 0,
		.file = NULL,
		.compress = 0,
		.hide_progress = 0,
	};

	NVME_ARGS_OUTPUT_FORMATS(opts, NORMAL, "Output format: normal",
		OPT_STRING("dump-type", 't', "TYPE", &cfg.type, type),
		OPT_INT("telemetry-data-area", 'a', &cfg.area, area),
		OPT_FILE("output-file", 'O', &cfg.file, file),
		OPT_FLAG("compress", 'z', &cfg.compress, compress),
		OPT_FLAG("hide-progress", 'H', &cfg.hide_progress, hide_progress));

	samsung_initialize();

	err = parse_and_open(&ctx, &hdl, argc, argv, desc, opts);
	if (err)
		return err;

	nvme_init_identify_ctrl(&cmd, &ctrl);
	err = libnvme_exec_admin_passthru(hdl, &cmd);
	if (err) {
		samsung_print_error(err);
		return err;
	}

	if (le16_to_cpu(ctrl.vid) != 0x144d) {
		samsung_print_error(SAMSUNG_GENERAL_INVALID_VID_ERROR);
		fprintf(stderr, "Current Vendor ID: 0x%X\n", le16_to_cpu(ctrl.vid));
		return SAMSUNG_GENERAL_INVALID_VID_ERROR;
	}

	get_serial_number(&ctrl, sn);

	/*
	 * -a applies to the telemetry dumps whether or not -t was given, so
	 * validate it before the dump type selection.
	 */
	tele_data_area = cfg.area;
	if (tele_data_area < 0 || tele_data_area > 4) {
		fprintf(stderr, "\nUnsupported data area entered. "
				"Valid range: 0 to 4.\n");
		samsung_print_error(SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR);
		return SAMSUNG_GENERAL_INVALID_PARAMETER_ERROR;
	}

	if (!cfg.type)
		select_all = true;
	else {
		char type_buf[SAMSUNG_FILE_NAME_SIZE] = {0,};
		char *tok, *saveptr = NULL;

		/*
		 * -t accepts a comma-separated list, e.g. "host0,ctlr,vendor".
		 * strtok_r() mutates its input, so parse a copy of cfg.type
		 * rather than the option string itself.
		 */
		snprintf(type_buf, sizeof(type_buf), "%s", cfg.type);

		for (tok = strtok_r(type_buf, ",", &saveptr); tok != NULL;
				tok = strtok_r(NULL, ",", &saveptr)) {
			err = vs_internal_log_select_type(tok, sel);
			if (err != 0) {
				samsung_print_error(err);
				return err;
			}
		}
	}

	/*
	 * -O is a file name prefix: get_file_path() concatenates it onto the
	 * generated name without inserting a separator, so "./dumps/" yields
	 * "./dumps/<sn>_..." and "./dumps/run1" yields "./dumps/run1<sn>_...".
	 * Either way the directories end at the last '/', which is what
	 * shr_mkdir_from_fname() creates. A bare "run1" has no directory
	 * part and reduces to creating ".", a harmless no-op.
	 */
	if (cfg.file) {
		err = shr_mkdir_from_fname(cfg.file, 0777);
		if (err < 0) {
			fprintf(stderr, "mkdir for %s: %s\n", cfg.file,
					strerror(-err));
			return SAMSUNG_GENERAL_FILE_OPEN_ERROR;
		}
	}

	if (cfg.compress) {
		err = insert_dir(cfg.file, "temp_samsung_dumps", dump_save_dir,
				sizeof(dump_save_dir));
		if (err != 0) {
			samsung_print_error(err);
			return err;
		}
	} else {
		snprintf(dump_save_dir, sizeof(dump_save_dir), "%s",
				cfg.file ? cfg.file : "./");
	}

	g_hide_progress = cfg.hide_progress;

	/*
	 * Collection is best effort: every dump is attempted and each failure
	 * is reported where it happens, so one unsupported or failing dump
	 * does not cost the others.
	 */

	/*
	 * Host(0) is intentionally excluded from "all" dumps (only run when
	 * explicitly selected), so it is gated on sel[] directly rather than
	 * WANT_DUMP()/select_all.
	 */
	if (sel[DUMP_TYPE_HOST_0]) {
		if (tele_data_area == 0)
			try_get_telemetry_all(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_HOST_0);
		else
			try_get_telemetry(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_HOST_0, tele_data_area, true);
	}

	if (WANT_DUMP(DUMP_TYPE_HOST_1)) {
		if (tele_data_area == 0)
			try_get_telemetry_all(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_HOST_1);
		else
			try_get_telemetry(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_HOST_1, tele_data_area, true);
	}

	if (WANT_DUMP(DUMP_TYPE_CTLR)) {
		if (tele_data_area == 0)
			try_get_telemetry_all(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_CTLR);
		else
			try_get_telemetry(hdl, &ctrl, dump_save_dir, sn,
					DUMP_TYPE_CTLR, tele_data_area, true);
	}

	if (WANT_DUMP(DUMP_TYPE_VENDOR)) {
		run_vendor_dump(get_vendor_crash_dump_0xf7, hdl,
				"Vendor Crash Dump", 0xF7, dump_save_dir, sn);
		run_vendor_dump(get_vendor_crash_dump_0xf8, hdl,
				"Vendor Crash Dump", 0xF8, dump_save_dir, sn);
		run_vendor_dump(get_vendor_memory_dump_0xf7, hdl,
				"Vendor Memory Dump", 0xF7, dump_save_dir, sn);
		run_vendor_dump(get_vendor_memory_dump_0xf8, hdl,
				"Vendor Memory Dump", 0xF8, dump_save_dir, sn);
		run_vendor_dump(get_vendor_debug_dump_0xf7, hdl,
				"Vendor Debug Dump", 0xF7, dump_save_dir, sn);
		run_vendor_dump(get_vendor_debug_dump_0xf8, hdl,
				"Vendor Debug Dump", 0xF8, dump_save_dir, sn);
	}

	printf("\n-------------------------------------------------------------\n");

	/*
	 * Archiving is the one step whose failure is reported through the
	 * exit status: without it the user does not have the file they asked
	 * -z for, while the dumps themselves are already on disk.
	 */
	err = 0;
	if (cfg.compress) {
		err = compress_dump_files(dump_save_dir, sn);
		if (err != 0)
			samsung_print_error(err);
	}

	printf("vs-internal-log done.\n");

	return err;
}

#undef WANT_DUMP

static struct command vs_internal_log_cmd = {
	.name = "vs-internal-log",
	.help = "Retrieve and save internal firmware log",
	.fn = vs_internal_log,
};

static struct command *commands[] = {
	&vs_internal_log_cmd,
	NULL,
};

static struct plugin plugin = {
	.name = "samsung",
	.desc = "Samsung vendor specific extensions",
	.version = SAMSUNG_PLUGIN_VERSION,
};

static void __shr_constructor register_plugin(void)
{
	plugin_add_group(&plugin, NULL, commands);
	register_extension(&plugin);
}
