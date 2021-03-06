#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "main.h"
#include "minipro.h"
#include "database.h"
#include "byte_utils.h"
#include "fuses.h"
#include "easyconfig.h"
#include "error.h"

struct {
	void (*action) (const char *, minipro_handle_t *handle, device_t *device);
	char *filename;
	device_t *device;
	enum { UNSPECIFIED = 0, CODE, DATA, CONFIG } page;
        int erase;
        int protect_off;
        int protect_on;
        int size_error;
        int size_nowarn;
        int icsp;
} cmdopts;

void print_help_and_exit(const char *progname) {
	char usage[] = 
		"Usage: %s [options]\n"
		"options:\n"
		"	-r <filename>	Read memory\n"
		"	-w <filename>	Write memory\n"
		"	-e 		Do NOT erase device\n"
		"	-u 		Do NOT disable write-protect\n"
		"	-P 		Do NOT enable write-protect\n"
		"	-p <device>	Specify device\n"
		"	-c <type>	Specify memory type (optional)\n"
		"			Possible values: code, data, config\n"
		"	-i		Use ICSP\n"
		"	-I		Use ICSP (without enabling Vcc)\n"
		"	-s		Error if file size does not match memory size\n"
		"	-S		No warning message for file size mismatch (can't combine with -s)\n";
	
	fprintf(stderr, usage, progname);
	exit(-1);
}

void print_devices_and_exit() {
	if(isatty(STDOUT_FILENO)) {
		// stdout is a terminal, opening pager
		char *pager_program = getenv("PAGER");
		if(!pager_program) pager_program = "less";
		FILE *pager = popen(pager_program, "w");
		dup2(fileno(pager), STDOUT_FILENO);
	}

	device_t *device;
	for(device = &(devices[0]); device[0].name; device = &(device[1])) {
		printf("%s\n", device->name);
	}
	exit(-1);
}

void parse_cmdline(int argc, char **argv) {
	char c;
	memset(&cmdopts, 0, sizeof(cmdopts));

	if(argc < 2) {
		print_help_and_exit(argv[0]);
	}

	while((c = getopt(argc, argv, "euPr:w:p:c:iIsS")) != -1) {
		switch(c) {
		        case 'e':
			  cmdopts.erase=1;  // 1= do not erase
			  break;

		        case 'u':
			  cmdopts.protect_off=1;  // 1= do not disable write protect
			  break;

		        case 'P':
			  cmdopts.protect_on=1;  // 1= do not enable write protect
			  break;
			  
			case 'p':
				if(!strcmp(optarg, "help"))
					print_devices_and_exit();
				cmdopts.device = get_device_by_name(optarg);
				if(!cmdopts.device)
					ERROR("Unknown device");
				break;
			case 'c':
				if(!strcmp(optarg, "code"))
					cmdopts.page = CODE;
				if(!strcmp(optarg, "data"))
					cmdopts.page = DATA;
				if(!strcmp(optarg, "config"))
					cmdopts.page = CONFIG;
				if(!cmdopts.page)
					ERROR("Unknown memory type");
				break;
			case 'r':
				cmdopts.action = action_read;
				cmdopts.filename = optarg;
				break;
			case 'w':
				cmdopts.action = action_write;
				cmdopts.filename = optarg;
				break;
			case 'S':
			       cmdopts.size_nowarn=1;
			       cmdopts.size_error=0;
			       break;

		        case 's':
			        cmdopts.size_error=1;
				break;
		        case 'i':
				cmdopts.icsp = MP_ICSP_ENABLE | MP_ICSP_VCC;
				break;
		        case 'I':
				cmdopts.icsp = MP_ICSP_ENABLE;
				break;
		}
	}
}

int get_file_size(const char *filename) {
	FILE *file = fopen(filename, "r");
	if(!file) {
		PERROR("Couldn't open file");
	}

	fseek(file, 0, SEEK_END);
	int size = ftell(file);
	fseek(file, 0, SEEK_SET);

	fclose(file);
	return(size);
}

void update_status(char *status_msg, char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	printf("\r\e[K%s", status_msg);
	vprintf(fmt, args);
	fflush(stdout);
}

int compare_memory(unsigned char *buf1, unsigned char *buf2, int size, unsigned char *c1, unsigned char *c2) {
	int i;
	for(i = 0; i < size; i++) {
		if(buf1[i] != buf2[i]) {
			*c1 = buf1[i];
			*c2 = buf2[i];
			return(i);
		}
	}
	return(-1);
}

/* RAM-centric IO operations */
void read_page_ram(minipro_handle_t *handle, unsigned char *buf, unsigned int type, const char *name, int size) {
	char status_msg[24];
	sprintf(status_msg, "Reading %s... ", name);

	device_t *device = handle->device;
	int blocks_count = size / device->read_buffer_size;
	if(size % device->read_buffer_size != 0) {
		blocks_count++;
	}

	int i;
	for(i = 0; i < blocks_count; i++) {
		update_status(status_msg, "%2d%%", i * 100 / blocks_count);
		// Translating address to protocol-specific
		int addr = i * device->read_buffer_size;
		if(device->opts4 & 0x2000) {
			addr = addr >> 1;
		}
		int len = device->read_buffer_size;
		// Last block
		if ((i + 1) * len > size)
			len = size % len;
		minipro_read_block(handle, type, addr, buf + i * device->read_buffer_size, len);
	}

	update_status(status_msg, "OK\n");
}

void write_page_ram(minipro_handle_t *handle, unsigned char *buf, unsigned int type, const char *name, int size) {
	char status_msg[24];
	sprintf(status_msg, "Writing %s... ", name);

	device_t *device = handle->device;
	
	int blocks_count = size / device->write_buffer_size;
	if(size % device->read_buffer_size != 0) {
		blocks_count++;
	}

	int i;
	for(i = 0; i < blocks_count; i++) {
		update_status(status_msg, "%2d%%", i * 100 / blocks_count);
		// Translating address to protocol-specific
		int addr = i * device->write_buffer_size;
		if(device->opts4 & 0x2000) {
			addr = addr >> 1;
		}
		int len = device->write_buffer_size;
		// Last block
		if ((i + 1) * len > size)
			len = size % len;
		minipro_write_block(handle, type, addr, buf + i * device->write_buffer_size, len);
	}
	update_status(status_msg, "OK\n");
}

/* Wrappers for operating with files */
void read_page_file(minipro_handle_t *handle, const char *filename, unsigned int type, const char *name, int size) {
	FILE *file = fopen(filename, "w");
	if(file == NULL) {
		PERROR("Couldn't open file for writing");
	}

	unsigned char *buf = malloc(size);
	if(!buf) {
		ERROR("Can't malloc");
	}

	read_page_ram(handle, buf, type, name, size);
	fwrite(buf, 1, size, file);

	fclose(file);
	free(buf);
}

void write_page_file(minipro_handle_t *handle, const char *filename, unsigned int type, const char *name, int size) {
	FILE *file = fopen(filename, "r");
	if(file == NULL) {
		PERROR("Couldn't open file for reading");
	}

	unsigned char *buf = malloc(size);
	if(!buf) {
		ERROR("Can't malloc");
	}

	// we already checked filesize going into write
	fread(buf, 1, size, file);
	write_page_ram(handle, buf, type, name, size);
	fclose(file);
	free(buf);
}

void read_fuses(minipro_handle_t *handle, const char *filename, fuse_decl_t *fuses) {
	printf("Reading fuses... ");
	fflush(stdout);
	if(Config_init(filename)) {
		PERROR("Couldn't create config");
	}

	minipro_begin_transaction(handle);
	int i, d;
	char data_length = 0, opcode = fuses[0].minipro_cmd;
	unsigned char buf[11];
	for(i = 0; fuses[i].name; i++) {
		data_length += fuses[i].length;
		if(fuses[i].minipro_cmd < opcode) {
			ERROR("fuse_decls are not sorted");
		}
		if(fuses[i+1].name == NULL || fuses[i+1].minipro_cmd > opcode) {
			minipro_read_fuses(handle, opcode, data_length, buf);
			// Unpacking received buf[] accordingly to fuse_decls with same minipro_cmd
			for(d = 0; fuses[d].name; d++) {
				if(fuses[d].minipro_cmd != opcode) {
					continue;
				}
				int value = load_int(&(buf[fuses[d].offset]), fuses[d].length, MP_LITTLE_ENDIAN);
				Config_set_int(fuses[d].name, value);
			}

			opcode = fuses[i+1].minipro_cmd;
			data_length = 0;
		}
	}
	minipro_end_transaction(handle);

	Config_close();
	printf("OK\n");
}

void write_fuses(minipro_handle_t *handle, const char *filename, fuse_decl_t *fuses) {
	printf("Writing fuses... ");
	fflush(stdout);
	if(Config_open(filename)) {
		PERROR("Couldn't open config");
	}

	minipro_begin_transaction(handle);
	int i, d;
	char data_length = 0, opcode = fuses[0].minipro_cmd;
	unsigned char buf[11];
	for(i = 0; fuses[i].name; i++) {
		data_length += fuses[i].length;
		if(fuses[i].minipro_cmd < opcode) {
			ERROR("fuse_decls are not sorted");
		}
		if(fuses[i+1].name == NULL || fuses[i+1].minipro_cmd > opcode) {
			for(d = 0; fuses[d].name; d++) {
				if(fuses[d].minipro_cmd != opcode) {
					continue;
				}
				int value = Config_get_int(fuses[d].name);
				format_int(&(buf[fuses[d].offset]), value, fuses[d].length, MP_LITTLE_ENDIAN);
			}
			minipro_write_fuses(handle, opcode, data_length, buf);

			opcode = fuses[i+1].minipro_cmd;
			data_length = 0;
		}
	}
	minipro_end_transaction(handle);

	Config_close();
	printf("OK\n");
}

void verify_page_file(minipro_handle_t *handle, const char *filename, unsigned int type, const char *name, int size) {
        int fsize;
	FILE *file = fopen(filename, "r");
	if(file == NULL) {
		PERROR("Couldn't open file for reading");
	}

	/* Loading file */
	int file_size = get_file_size(filename);
	unsigned char *file_data = malloc(file_size);
	// already checked file size
	fread(file_data, 1, file_size, file);
	fclose(file);

	minipro_begin_transaction(handle);

	/* Downloading data from chip*/
	unsigned char *chip_data = malloc(fsize);
	read_page_ram(handle, chip_data, type, name, size);
	unsigned char c1, c2;
	int idx = compare_memory(file_data, chip_data, file_size, &c1, &c2);

	/* No memory leaks */
	free(file_data);
	free(chip_data);

	if(idx != -1) {
		ERROR2("Verification failed at 0x%02x: 0x%02x != 0x%02x\n", idx, c1, c2);
	} else {
		printf("Verification OK\n");
	}

	minipro_end_transaction(handle);
}

/* Higher-level logic */
void action_read(const char *filename, minipro_handle_t *handle, device_t *device) {
	char *code_filename = (char*) filename;
	char *data_filename = (char*) filename;
	char *config_filename = (char*) filename;
	char default_data_filename[] = "eeprom.bin";
	char default_config_filename[] = "fuses.conf";

	minipro_begin_transaction(handle); // Prevent device from hanging
	switch(cmdopts.page) {
		case UNSPECIFIED:
			data_filename = default_data_filename;
			config_filename = default_config_filename;
		case CODE:
			read_page_file(handle, code_filename, MP_READ_CODE, "Code", device->code_memory_size);
			if(cmdopts.page) break;
		case DATA:
			if(device->data_memory_size) {
				read_page_file(handle, data_filename, MP_READ_DATA, "Data", device->data_memory_size);
			}
			if(cmdopts.page) break;
		case CONFIG:
			if(device->fuses) {
				read_fuses(handle, config_filename, device->fuses);
			}
			break;
	}
	minipro_end_transaction(handle); 
}

void action_write(const char *filename, minipro_handle_t *handle, device_t *device) {
        int fsize;
	switch(cmdopts.page) {
		case UNSPECIFIED:
		case CODE:
		  fsize=get_file_size(filename);
		  if (fsize != device->code_memory_size) {
		    if (cmdopts.size_error)
		      ERROR2("Incorrect file size: %d (needed %d)\n", fsize, device->code_memory_size);
		    else
		      if (cmdopts.size_nowarn==0) printf("Warning: Incorrect file size: %d (needed %d)\n", fsize, device->code_memory_size);
	          }
		  break;
		case DATA:
		  fsize=get_file_size(filename);
		  if (fsize != device->data_memory_size) {
		    if (cmdopts.size_error)
		      ERROR2("Incorrect file size: %d (needed %d)\n", fsize, device->data_memory_size);
		    else
		      if (cmdopts.size_nowarn==0) printf("Warning: Incorrect file size: %d (needed %d)\n", fsize, device->data_memory_size);
	          }
			break;
		case CONFIG:
			break;
	}
	minipro_begin_transaction(handle);
	if (cmdopts.erase==0)
	  {
		minipro_prepare_writing(handle);
		minipro_end_transaction(handle); // Let prepare_writing() to take an effect
	  }

	minipro_begin_transaction(handle);
	minipro_get_status(handle);
	if (cmdopts.protect_off==0 && device->opts4 & 0xc000) {
		minipro_protect_off(handle);
	}

	switch(cmdopts.page) {
		case UNSPECIFIED:
		case CODE:
			write_page_file(handle, filename, MP_WRITE_CODE, "Code", device->code_memory_size);
			verify_page_file(handle, filename, MP_READ_CODE, "Code", device->code_memory_size);
			break;
		case DATA:
			write_page_file(handle, filename, MP_WRITE_DATA, "Data", device->data_memory_size);
			verify_page_file(handle, filename, MP_READ_DATA, "Data", device->data_memory_size);
			break;
		case CONFIG:
			if(device->fuses) {
				write_fuses(handle, filename, device->fuses);
			}
			break;
	}
	minipro_end_transaction(handle); // Let prepare_writing() to make an effect

	if (cmdopts.protect_on==0 && device->opts4 & 0xc000) {
		minipro_begin_transaction(handle);
		minipro_protect_on(handle);
		minipro_end_transaction(handle);
	}
}

int main(int argc, char **argv) {
	parse_cmdline(argc, argv);
	if(!cmdopts.filename) {
		print_help_and_exit(argv[0]);
	}
	if(cmdopts.action && !cmdopts.device) {
		USAGE_ERROR("Device required");
	}

	device_t *device = cmdopts.device;
	minipro_handle_t *handle = minipro_open(device);
	handle->icsp = cmdopts.icsp;

	// Printing system info
	minipro_system_info_t info;
	minipro_get_system_info(handle, &info);
	printf("Found Minipro %s v%s\n", info.model_str, info.firmware_str);

	// Verifying Chip ID (if applicable)
	if(device->chip_id_bytes_count && device->chip_id) {
		minipro_begin_transaction(handle);
		unsigned int chip_id = minipro_get_chip_id(handle);
		if (chip_id == device->chip_id) {
			printf("Chip ID OK: 0x%02x\n", chip_id);
		} else {
			ERROR2("Invalid Chip ID: expected 0x%02x, got 0x%02x\n", device->chip_id, chip_id);
		}		
		minipro_end_transaction(handle);
	}

	/* TODO: put in devices.h and remove this stub */
	switch(device->protocol_id) {
		case 0x71:
			device->fuses = avr_fuses;
			break;
 	        case 0x10063:   //  select 2 fuses
		  device->fuses=pic2_fuses;
		  device->protocol_id&=0xFFFF;
		  break;
		  
		case 0x63:
		case 0x65:
			device->fuses = pic_fuses;
			break;
	}

	cmdopts.action(cmdopts.filename, handle, device);

	minipro_close(handle);

	return(0);
}
