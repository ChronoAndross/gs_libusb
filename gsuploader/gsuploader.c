#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if _WIN64 || _WIN32
#include "libusb.h"
#include <io.h>
#else
#include <libusb-1.0/libusb.h>
#include <unistd.h>
#endif

#include "codegen.h"
#include "gscomms.h"

int upload_cb(gscomms * g, code_block * cb, unsigned long address);

// #define NEON64_MODE 1 - No longer needed as neon64 mode is automated now.

// Change defines below to represent older style arguments (no defined load points)
//#if NEON64_MODE
#define DEFAULT_NEON64_UPLOAD_ADDR 0x80300000UL
#define DEFAULT_NEON64_ENTRYPOINT  0x80300000UL
//#else
#define DEFAULT_UPLOAD_ADDR 0x80000400UL // Can use cached area, works great when the code loading sets up cache properly!
#define DEFAULT_ENTRYPOINT  0x80000400UL
//#endif

//GLOBALS BAD!
unsigned long UPLOAD_ADDR = DEFAULT_NEON64_UPLOAD_ADDR;
unsigned long ENTRYPOINT = DEFAULT_NEON64_ENTRYPOINT;

#define EMBED_ADDR  0xA0800000UL-1024 // Relocate the preloader code to end of EXP RAM with 1KB allocation (seems safe enough.)

// GS Code Handler(uncached)
#define INSN_PATCH_ADDR 0xA07C5C00UL

// call to receive byte in GS upload
#define GET_BYTE_PATCH_ADDR 0xA07919B0UL

// flag: use 2x transfer?
#define USE_FAST_RECEIVE 1 // Changed 0->1 - seems more stable

// flag: use bulk 2x transfer?
#define USE_BULK_RECEIVE 0 // Changed 1->0 - seems more stable

#define EEPROM_START 0x80207700UL
#define EEPROM_SIZE 512

FILE* fopen_g(const char* inFileName, const char* inMode)
{
	FILE* outFile = NULL;
#if _WIN64 || _WIN32
	fopen_s(&outFile, inFileName, inMode);
#else
	outFile = fopen(inFileName, inMode);
#endif
	return outFile;
}

int SendInterrupt(gscomms* g, unsigned long* setup_addr, unsigned char* save_buffer, 
	unsigned long memory_start, unsigned long file_size, int write_file)
{
	if (!InitGSCommsNoisy(g, RETRIES, 1)) {
		printf("Init failed\n");
		do_clear(g);
		return 1;
	}

#if USE_FAST_RECEIVE || USE_BULK_RECEIVE
	unsigned long byte_loader_addr;
#endif

	{
		// generate embedded code
		code_block *setup_cb = generate_setup(ENTRYPOINT, INSN_PATCH_ADDR);
#if USE_FAST_RECEIVE
		code_block *receive_cb = generate_2x_receive();
#elif USE_BULK_RECEIVE
		code_block *receive_cb = generate_bulk_receive();
#endif

		// upload embedded code
		unsigned long embed_addr = EMBED_ADDR;

		upload_cb(g, setup_cb, embed_addr);
		*setup_addr = embed_addr;
		embed_addr += setup_cb->size;
		embed_addr = (embed_addr + 3) / 4 * 4;

#if USE_FAST_RECEIVE || USE_BULK_RECEIVE
		upload_cb(g, receive_cb, embed_addr);
		byte_loader_addr = embed_addr;
		embed_addr += receive_cb->size;
		embed_addr = (embed_addr + 3) / 4 * 4;
#endif

		free_cb(setup_cb);
#if USE_FAST_RECEIVE || USE_BULK_RECEIVE
		free_cb(receive_cb);
#endif
	}

#if USE_FAST_RECEIVE || USE_BULK_RECEIVE
	{

		code_block * recv_jal_cb = generate_jal(byte_loader_addr, "upload driver patch");

		upload_cb(g, recv_jal_cb, GET_BYTE_PATCH_ADDR);

		free_cb(recv_jal_cb);
	}

#if USE_FAST_RECEIVE
	set_mode(g, GSCOMMS_MODE_FAST);
#elif USE_BULK_RECEIVE
	set_mode(g, GSCOMMS_MODE_BULK);
#endif 

#if 1
	// might take a little bit for the instruction cache to turn over
	Disconnect(g);
	sleep(1);
	if (!InitGSComms(g, RETRIES)) {
		printf("Init failed\n");
		do_clear(g);
		return 1;
	}
#endif

#endif
	// Read save data from Gameshark after the interrupt hijacking completes.
	// WARNING: Reading off of live memory can be risky so proceed with caution!
	if (save_buffer)
	{
		printf("Reading memory from cart.\n");
		ReadRAM(g, save_buffer, memory_start, file_size);
		printf("Reading memory complete.\n");

		if (write_file)
		{
			printf("Writing file...\n");
			FILE* save_file = fopen_g("output.txt", "wb");
			fwrite(save_buffer, sizeof(unsigned char), file_size, save_file);
			fclose(save_file);
			printf("File writing complete.\n");
		}
	}
	return 0;
}

// Wrapper for Android
void ReadN64RAM(unsigned long memory_start, unsigned long file_size)
{
	gscomms* g = setup_gscomms();
	unsigned long setup_addr;
	unsigned char* save_buffer = malloc(file_size);
	memset((void*)save_buffer, 0x00, file_size);

	int failed = SendInterrupt(g, &setup_addr, save_buffer, memory_start, file_size, 0/*dont write the file*/);
	if (!failed)
		; // TODO: send back to android so we can write a file based on this memory in that code.
	free(save_buffer);
}

int main(int argc, char ** argv)
{
  gscomms * g = NULL;
  int two_stage = 0;

  FILE * infile1 = NULL;
  FILE * infile2 = NULL;

  // commandline
  printf("\nN64 HomeBrew Loader - hcs, ppcasm\n");
  printf("MCS7705 USB version via libusb\n\n");

  if (argc == 2 || argc == 4) {
    two_stage = 0;
    if(argc == 4) {
       UPLOAD_ADDR = (unsigned long)strtol(argv[2], NULL, 16);
       ENTRYPOINT = (unsigned long )strtol(argv[3], NULL, 16);
    }
    else {
       UPLOAD_ADDR = DEFAULT_UPLOAD_ADDR;
       ENTRYPOINT = DEFAULT_ENTRYPOINT;
    }

    printf("Single-Stage Loader - UPLOAD_ADDR: 0x%08x | ENTRYPOINT: 0x%08x\n\n", (unsigned int)UPLOAD_ADDR, (unsigned int)ENTRYPOINT);
  }
  else if (argc == 3 || argc == 5) {
    two_stage = 1;
    if(argc == 5) {
       UPLOAD_ADDR = (unsigned long)strtol(argv[3], NULL, 16);
       ENTRYPOINT = (unsigned long )strtol(argv[4], NULL, 16);
    }
    else {
       UPLOAD_ADDR = DEFAULT_NEON64_UPLOAD_ADDR;
       ENTRYPOINT = DEFAULT_NEON64_ENTRYPOINT;
    }

    printf("Two-Stage Loader - UPLOAD_ADDR: 0x%08x | ENTRYPOINT: 0x%08x\n\n", (unsigned int)UPLOAD_ADDR, (unsigned int)ENTRYPOINT);
  }
  else
  {
    printf("Wrong Usage:\n(Homebrew Uploader): %s <binary>\n", argv[0]);
    printf("(NEON64GS mode/Two Stage Loader): %s <NEON64GS.BIN/loader> <ROM/binary>\n\n", argv[0]);
    printf("Alternatively you can specify some addresses using the following:\n\n");
    printf("(Homebrew Uploader): %s <binary> <upload_address> <_start/entrypoint> <transfer_mode: 0 (BULK) | 1 (FAST)\n", argv[0]);
    printf("(NEON64GS mode/Two Stage Loader): %s <NEON64GS.BIN/loader> <ROM/binary> <upload_address> <_start/entrypoint> <transfer_mode: 0 (BULK) | 1 (FAST)\n\n", argv[0]);
    return 1;
  }

  // open inputs
  infile1 = fopen_g(argv[1], "rb");
  if(!infile1)
  {
    printf("error opening %s\n", argv[1]);
    //do_clear(g); - Not needed, gscomms isn't actually initialized yet and it'll sigsev on fopen error
    return 1;
  }

  if (two_stage) {
    infile2 = fopen_g(argv[2], "rb");
    if(!infile2)
    {
      printf("error opening %s\n", argv[2]);
      //do_clear(g); - Not needed, gscomms isn't actually initialized yet and it'll sigsev on fopen error
      return 1;
    }
  }

  // get in touch with the GS
  g = setup_gscomms();
  unsigned long setup_addr;
  unsigned char* save_buffer = malloc(EEPROM_SIZE);
  memset((void*)save_buffer, 0x00, EEPROM_SIZE);

  int failed = SendInterrupt(g, &setup_addr, save_buffer, EEPROM_START, EEPROM_SIZE, 1/*write the file*/);
  free(save_buffer);

  if (failed)
	  return failed;


  /*Upload binary to specified address.*/

  WriteRAMfromFile(g, infile1, UPLOAD_ADDR, -1);
  fclose(infile1);

  {
    code_block * setup_j_cb = generate_jump(setup_addr, "patch into code list");
    upload_cb(g, setup_j_cb, INSN_PATCH_ADDR);
    free_cb(setup_j_cb);
  }

  Disconnect(g);

  do_clear(g);

  printf("Done.\n");


  if (two_stage) {
    /* let the loader get settled */
    printf("Send second stage in 1 second...\n");
    sleep(1);

    if (!InitGSComms(g, RETRIES)) {
      printf("Init failed\n");
      do_clear(g);
      return 1;
    }

    WriteRAMfromFile(g, infile2, UPLOAD_ADDR, -1);

    fclose(infile2);

    Disconnect(g);
  }

  cleanup_gscomms(g);
  g = NULL;

  return 0;
}

int upload_cb(gscomms * g, code_block * cb, unsigned long address) {
  printf("Send %s\n", cb->name);
  return WriteRAM(g, cb->code, address, cb->size);
}
