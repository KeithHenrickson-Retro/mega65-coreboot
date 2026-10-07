/*
  Simple minicom like terminal program for the MEGA65

*/
#include "includes.h"

#include <string.h>

#include "uart.h"
#include "c65reboot.h"

#include "cbm.h"
#include "c64.h"

uint8_t old_54;
uint8_t startedin_c64;

void print_box(unsigned char x1, unsigned char y1,
	       unsigned char x2, unsigned char y2,
	       unsigned char colour)
{
  uint16_t char_addr = 0xf000 + y1 * 80;
  for(int x=x1;x<=x2;x++) {
    POKE(char_addr+x,0x20);
    lpoke(0xff80000L - 0xf000 + char_addr+x, 0x20 | colour);    
  }
  for(int y=y1+1;y<y2;y++) {
    char_addr+=80;
    POKE(char_addr+x1,0x20);
    lpoke(0xff80000L - 0xf000 + char_addr+x1, 0x20 | colour);    
    POKE(char_addr+x2,0x20);
    lpoke(0xff80000L - 0xf000 + char_addr+x2, 0x20 | colour);        
  }
  char_addr+=80;
  for(int x=x1;x<=x2;x++) {
    POKE(char_addr+x,0x20);
    lpoke(0xff80000L - 0xf000 + char_addr+x, 0x20 | colour);    
  }
  
}

#include "ascii-font.c"

void print_text40(unsigned char x, unsigned char y, unsigned char colour, unsigned char clear_line, const char *msg)
{
  uint16_t addr = 0x0400 + x + y * 40;
  uint16_t caddr = 0xD800 + x + y * 40;
  if (clear_line) {
    for (int i = 0; i < 40; i++) {
      POKE(addr + i, ' ');
      POKE(caddr, colour);
    }
  }
  while (*msg && x < 40) {
    POKE(addr, *msg);
    POKE(caddr, colour);
    msg++;
    addr++;
    caddr++;
    x++;
  }
}

void c64_40col_mode(void)
{
  old_54 = PEEK(0xD054);

  // Switch to standard lowercase/uppercase C64 mode
  POKE(0xD031, 0x40);

  POKE(0xD018, 0x16); // Screen RAM $0400, Char ROM $1800
  POKE(0xD058, 40);   // Logical row width
  POKE(0xD05E, 40);
  POKE(0xD011, PEEK(0xD011) & ~0x80); // clear high bit of raster
  POKE(0xD016, 0xC8); // 40 cols

  // Clear screen
  for(int i=0; i<1000; i++) {
    POKE(0x0400 + i, 0x20); // space
    POKE(0xD800 + i, 0x01); // white
  }

  // Use our ASCII charset (set last to avoid hot reg problems)
  lcopy((uint16_t)&ascii_font[0],0xe000,4096);
  POKE(0xD068,0x00);
  POKE(0xD069,0xE0);

  sethotregs(0);
}

void visual_bell(void)
{
  POKE(0xD020,0x01);
  for(int i=0;i<10;i++) {
    uint8_t old = PEEK(0xD7FA);
    while (PEEK(0xD7FA)==old) continue;
  }
  POKE(0xD020,0x00);
}

struct baud_rate {
  uint32_t baud;
  char baud_str[9];
  uint32_t baud_divisor;
};

#define NUM_BAUD_RATES 1
struct baud_rate baud_list[NUM_BAUD_RATES]={
  {115200,"115200", 40500000 / 115200 + 1},
};

uint8_t current_baud_rate = 0;
uint8_t current_uart = 0;

void send_cmd(char *cmd) {
  modem_uart_write((uint8_t*)cmd, strlen(cmd));
  uint8_t cr = '\r';
  modem_uart_write(&cr, 1);
}

void sleep_approx_1s() {
  uint8_t last_raster = PEEK(0xD012);
  int frames = 0;
  while (frames < 50) {
    uint8_t current_raster = PEEK(0xD012);
    if (current_raster < last_raster) frames++;
    last_raster = current_raster;
  }
}

uint8_t any_key_pressed(void) {
  for (uint8_t i = 0; i < 9; i++) {
    POKE(0xD614, i);
    uint8_t keys = PEEK(0xD613);
    if (keys != 0xff) {
      return 1;
    }
  }
  return 0;
}

uint8_t impatient_sleep(int approx_seconds) {
  uint8_t last_raster = PEEK(0xD012);
  int frames = 0;
  while (frames < (50 * approx_seconds)) {
    uint8_t current_raster = PEEK(0xD012);
    if (current_raster < last_raster) frames++;
    last_raster = current_raster;
    if (any_key_pressed()) {
      return 1;
    }
  }
  return 0;
}

char num_str[10];
void decode_num(uint16_t num) {
  if (num < 10) {
    num_str[0] = ' '; num_str[1] = ' '; num_str[2] = '0' + num; num_str[3] = 0;
  } else if (num < 100) {
    num_str[0] = ' '; num_str[1] = '0' + (num/10); num_str[2] = '0' + (num%10); num_str[3] = 0;
  } else {
    num_str[0] = '0' + (num/100); num_str[1] = '0' + ((num/10)%10); num_str[2] = '0' + (num%10); num_str[3] = 0;
  }
}

void wait_for_modem() {
  char buf[80];
  int buf_idx = 0;
  
  while (1) {
    print_text40(0, 6, 0x0a, 1, "Waiting for JTAG modem to respond...");
    send_cmd("ATZ");
    
    uint32_t frames_passed = 0;
    uint8_t last_raster = PEEK(0xD012);
    int got_ok = 0;

    // wait up to 100 frames (2 seconds)
    while(frames_passed < 100) {
      uint8_t current_raster = PEEK(0xD012);
      if (current_raster < last_raster) frames_passed++;
      last_raster = current_raster;

      uint8_t rx_buf[64];
      uint16_t count = modem_uart_read(rx_buf, sizeof(rx_buf));
      if (count > 0) {
        frames_passed = 0; // reset timeout on receiving character
        for (uint16_t i=0; i<count; i++) {
          uint8_t c = rx_buf[i];
          if (c == '\r' || c == '\n') {
            if (buf_idx > 0) {
              buf[buf_idx] = 0;
              if (strncmp(buf, "OK READY", 8) == 0) {
                got_ok = 1;
              }
              buf_idx = 0;
            }
          } else {
            if (buf_idx < 79) buf[buf_idx++] = c;
          }
        }
      }
      if (got_ok) break;
    }
    if (got_ok) {
      print_text40(0, 6, 0x05, 1, "Modem OK!");
      break;
    }
    print_text40(0, 6, 0x03, 1, "Retrying...");
    sleep_approx_1s();
  }
}

#define MAX_CORES 50
typedef struct coreinfo {
  char title[30];
  char path[30];
  char number[10];
} coreinfo_t;

coreinfo_t cores[MAX_CORES];
int core_count = 0;

int parse_core_num(const char* str) {
  int num = 0;
  while(*str == ' ' || *str == '\t') str++;
  while(*str >= '0' && *str <= '9') {
    num = num * 10 + (*str - '0');
    str++;
  }
  return num;
}

void set_max_send(uint16_t max_bytes) {
  char max_send_command[20];
  char buf[128];
  int buf_idx = 0;
  int line_count = 0;

  uint32_t frames_passed = 0;

  print_text40(0, 3, 0x07, 0, "Setting Max Send...                     ");
  strcpy(max_send_command, "AT+COREMAX=");
  if (max_bytes < 10) {
    max_send_command[11] = '0' + max_bytes; max_send_command[12] = 0;
  } else if (max_bytes < 100) {
    max_send_command[11] = '0' + (max_bytes/10); max_send_command[12] = '0' + (max_bytes%10); max_send_command[13] = 0;
  } else {
    max_send_command[11] = '0' + (max_bytes/100); max_send_command[12] = '0' + ((max_bytes/10)%10); max_send_command[13] = '0' + (max_bytes%10); max_send_command[14] = 0;
  }
  strcat(max_send_command, "\n");
  send_cmd(max_send_command);

  uint8_t last_raster = PEEK(0xD012);
  int done = 0;

  while(frames_passed < 250 && !done) {
    uint8_t current_raster = PEEK(0xD012);
    if (current_raster < last_raster) frames_passed++;
    last_raster = current_raster;

    uint8_t rx_buf[64];
    uint16_t count = modem_uart_read(rx_buf, sizeof(rx_buf));
    if (count > 0) {
      frames_passed = 0;
      for (uint16_t i=0; i<count; i++) {
        uint8_t c = rx_buf[i];
        if (c == '\r' || c == '\n') {
          if (buf_idx > 0) {
            buf[buf_idx] = 0;
            if (strncmp(buf, "OK", 2) == 0 || strncmp(buf, "ERROR", 5) == 0) {
              done = 1;
            }
            buf_idx = 0;
          }
        } else {
          if (buf_idx < 127) buf[buf_idx++] = c;
        }
      }
    }
  }
}

void parse_core(const char* core_detail) {
  char *p = strstr(core_detail, "index=");
  if (p) {
    int num = parse_core_num(p + 6);
    if (num > 0 && core_count < MAX_CORES) {
      char *kind = strstr(core_detail, "kind=");
      char *path_ptr = strstr(core_detail, "path=\"");
      char *title = strstr(core_detail, "title=\"");

      // Skip directories as we already got them!
      int is_dir = 0;
      if (kind && strncmp(kind + 5, "DIR", 3) == 0) is_dir = 1;

      if (!is_dir) {
        if (title) {
          char *start = title + 7;
          char *end = strchr(start, '"');
          if (end && end > start) {
              int len = end - start;
              if (len > 29) len = 29;
              strncpy(cores[core_count].title, start, len);
            }
          if (path_ptr) {
            start = path_ptr + 6;
            end = strchr(start, '"');
            if (end) {
              int len = end - start;
              if (len > 29) len = 29;
              strncpy(cores[core_count].path, start, len);
            }
          }
        }
        decode_num(num);
        strcpy(cores[core_count].number, num_str);
        core_count++;
      }
    }
  }
}

void read_cores(const char* path) {
  core_count = 0;

  set_max_send(150);

  char cmd[128];
  strcpy(cmd, "AT+CORELIST");
  if (strcmp(path, "/") != 0) {
    strcat(cmd, "=");
    strcat(cmd, path);
  }

  print_text40(0, 3, 0x07, 0, "Querying directories...                 ");
  send_cmd(cmd);

  static char buf[512];
  int buf_idx = 0;
  uint32_t frames_passed = 0;
  uint8_t last_raster = PEEK(0xD012);
  int done = 0;

  // PASS 1: AT+CORELIST to get DIRs
  while(frames_passed < 250 && !done) {
    uint8_t current_raster = PEEK(0xD012);
    if (current_raster < last_raster) frames_passed++;
    last_raster = current_raster;

    uint8_t rx_buf[64];
    uint16_t count = modem_uart_read(rx_buf, sizeof(rx_buf));
    if (count > 0) {
      frames_passed = 0;
      for (uint16_t i=0; i<count; i++) {
        uint8_t c = rx_buf[i];
        if (c == '\r' || c == '\n') {
          if (buf_idx > 0) {
            buf[buf_idx] = 0;
            if (strncmp(buf, "END", 3) == 0) {
              done = 1;
            } else if (strncmp(buf, "CONT", 4) == 0) {
              send_cmd("AT+COREMORE");
            } else if (strncmp(buf, "AT+CORELIST", 11) != 0 && strncmp(buf, "OK", 2) != 0) {
              int num = parse_core_num(buf);
              if (num > 0 && core_count < MAX_CORES) {
                char *dir_ptr = strstr(buf, "DIR ");
                if (dir_ptr) {
                  char *p = dir_ptr + 4;
                  while(*p == ' ') p++;
                  while(*p >= '0' && *p <= '9') p++; // skip size
                  while(*p == ' ') p++;
                  if (*p == '-') {
                    p++;
                    while(*p == ' ') p++;
                  } else if (*p >= '0' && *p <= '9') {
                    while(*p && *p != ' ') p++; // skip date
                    while(*p == ' ') p++;
                    while(*p && *p != ' ') p++; // skip time
                    while(*p == ' ') p++;
                  }
                  if (*p) {
                    char nice[80] = {0};
                    char num_str[10];
                    if (num < 10) {
                      num_str[0] = ' '; num_str[1] = ' '; num_str[2] = '0' + num; num_str[3] = 0;
                    } else if (num < 100) {
                      num_str[0] = ' '; num_str[1] = '0' + (num/10); num_str[2] = '0' + (num%10); num_str[3] = 0;
                    } else {
                      num_str[0] = '0' + (num/100); num_str[1] = '0' + ((num/10)%10); num_str[2] = '0' + (num%10); num_str[3] = 0;
                    }
                    strcpy(nice, num_str);
                    strcat(nice, " DIR  ");
                    strncat(nice, p, 79 - strlen(nice));
                  }
                }
              }
            }
            buf_idx = 0;
          }
        } else {
          if (buf_idx < 511) buf[buf_idx++] = c;
        }
      }
    }
  }

  // PASS 2: AT+COREDETAIL to get COREs
  strcpy(cmd, "AT+COREDETAIL");
  if (strcmp(path, "/") != 0) {
    strcat(cmd, "=");
    strcat(cmd, path);
  }
  print_text40(0, 3, 0x07, 0, "Querying cores...                       ");
  send_cmd(cmd);

  buf_idx = 0;
  frames_passed = 0;
  last_raster = PEEK(0xD012);
  done = 0;

  static char detail_buf[512];
  memset(detail_buf, 0, sizeof(detail_buf));
  uint8_t appending = 0;
  while(frames_passed < 250 && !done) {
    uint8_t current_raster = PEEK(0xD012);
    if (current_raster < last_raster) frames_passed++;
    last_raster = current_raster;

    uint8_t rx_buf[64];
    uint16_t count = modem_uart_read(rx_buf, sizeof(rx_buf));
    if (count > 0) {
      frames_passed = 0;
      for (uint16_t i=0; i<count; i++) {
        uint8_t c = rx_buf[i];
        if (c == '\r' || c == '\n') {
          if (buf_idx > 0) {
            buf[buf_idx] = 0;
            if (strncmp(buf, "END", 3) == 0) {
              done = 1;
            } else if (strncmp(buf, "CONT", 4) == 0) {
              send_cmd("AT+COREMORE");
            } else if (strncmp(buf, "+COREDETAIL:", 12) == 0) {
              strncpy(detail_buf, buf, sizeof(detail_buf) - 1);
              detail_buf[sizeof(detail_buf) - 1] = 0;
              if (detail_buf[strlen(detail_buf) - 1] == '\\') {
                appending = 1;
                detail_buf[strlen(detail_buf) - 1] = 0;
              } else {
                parse_core(detail_buf);
              }
            } else if ((strncmp(buf, "AT+COREMORE", 11) != 0) && (strncmp(buf, "OK", 2) != 0) && appending) {
              strncat(detail_buf, buf, sizeof(detail_buf) - 1);
              detail_buf[sizeof(detail_buf) - 1] = 0;
              if (detail_buf[strlen(detail_buf) - 1] == '\\') {
                detail_buf[strlen(detail_buf) - 1] = 0;
              } else {
                appending = 0;
                parse_core(detail_buf);
              }
            }
            buf_idx = 0;
          }
        } else {
          if (buf_idx < 511) buf[buf_idx++] = c;
        }
      }
    }
  }

  set_max_send(0);
}

uint8_t partial_match(char *core_name, char *match_name) {
  char *core_ptr = core_name;
  char *match_ptr = match_name;

  if ((core_ptr == NULL) || (match_ptr == NULL)) {
    return 0;
  }
  while (*match_ptr != 0) {
    if (*match_ptr == '*') {
      return 1;
    }
    if (*core_ptr != 0) {
      if ((*core_ptr == *match_ptr) || (*match_ptr == '?')) {
        core_ptr++;
	match_ptr++;
      } else {
        return 0;
      }
    }
  }
  return 1;
}

char conf_buffer[255];
uint8_t conf_buffer_len = 0;
uint8_t match_path = 0;
char match_string[50];
uint8_t match_string_len = 0;

uint8_t petscii_ascii[] = {
// 0
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
// 32
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x35, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
// 64
  0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
  0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,

// 96
  0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
  0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,

// 128
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

// 160
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

// 192
  0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
  0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,

// 224
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

__attribute__((noreturn)) void universal_reset(void) {
  while (any_key_pressed()) {
    POKE(0xD610, 0);
  }
  while (PEEK(0xD610) != 0) {
    POKE(0xD610, 0);
  }

  POKE(0xD054, old_54 & 0x20);

  if (startedin_c64) {
    __attribute__((leaf)) asm volatile(
        "sei \n"
    	"lda #133 \n"
    	"sta $01 \n"
     	"lda #$00 \n"
     	"ldx #$E3 \n"
     	"ldy #$00 \n"
     	"ldz #$B3 \n"
     	"map \n"
     	"eom \n"
        "lda #$00 \n"
        "jmp $ff53" : : : "a", "x", "y", "c", "v");
  } else {
    for (size_t i = 0; i < sizeof(bin2c_c65reboot_bin); i++) {
      POKE(0x0380 + i, bin2c_c65reboot_bin[i]);
    }
    __attribute__((leaf)) asm volatile(
      "sei \n"
      "jmp $0380" : : : "a", "x", "y", "c", "v");
  }
  while(1);
}

void clear_error_channel(void) {
  cbm_k_setlfs(15, 8, 15);
  cbm_k_setnam("");
  cbm_k_open();

  if (cbm_k_readst() != 0) {
    cbm_k_close(15);
    return;
  }

  cbm_k_chkin(15);

  uint8_t counter = 0;
  do {
    uint8_t next_char = cbm_k_chrin();
    uint8_t st_val = cbm_k_readst();
    if (st_val != 0) {
      cbm_k_close(15);
      return;
    }
    counter++;
  } while (counter < 254);
  cbm_k_close(15);

  POKE(0xD02fL, 0x47);
  POKE(0xd02fL, 0x53);
}

void read_conf_file(void) {
  print_text40(0, 3, 1, 2, "Loading config file...");

  cbm_k_setlfs(2, 8, 2);
  cbm_k_setnam("CORE CONFIG,S,R");
  cbm_k_open();

  if (cbm_k_readst() != 0) {
    cbm_k_close(2);
    clear_error_channel();

    POKE(0xD02fL, 0x47);
    POKE(0xd02fL, 0x53);

    print_text40(0, 4, 1, 2, "Unable to open config file.");
    print_text40(0, 5, 1, 2, "Press any key or wait to exit.");
    impatient_sleep(25);
    universal_reset();
    return;
  }

  cbm_k_chkin(2);

  do {
    uint8_t next_char = cbm_k_chrin();
    uint8_t st_val = cbm_k_readst();
    if (st_val != 0) {
      if (st_val == 64) {
        // EOF
        break;
      }
      clear_error_channel();

      POKE(0xD02fL, 0x47);
      POKE(0xd02fL, 0x53);

      print_text40(0, 4, 2, 1, "Unable to open config file.");
      print_text40(0, 5, 2, 1, "Press any key or wait to exit.");
      impatient_sleep(25);
      universal_reset();
    }
    conf_buffer[conf_buffer_len] = next_char;
    conf_buffer_len++;
  } while (conf_buffer_len < 254);
  conf_buffer[conf_buffer_len] = 0;
  cbm_k_close(2);
}

void parse_conf_file(void) {
  char *pchar;
  char *title = strstr(conf_buffer, "TITLE=");

  if (title) {
    pchar = title + 6;
  } else {
    POKE(0xD02fL, 0x47);
    POKE(0xd02fL, 0x53);

    print_text40(0, 4, 2, 1, "Invalid config file.");
    print_text40(0, 5, 2, 1, "Press any key or wait to exit.");
    impatient_sleep(25);
    universal_reset();
  }

  if (pchar != NULL) {
    char cbmchar = *pchar;
    while ((cbmchar != 0) && (cbmchar != 13)) {
      char asciichar = petscii_ascii[(uint8_t)cbmchar];
      if (asciichar != 0) {
        if (match_string_len < 49) {
          match_string[match_string_len++] = asciichar;
        }
      }
      pchar++;
      cbmchar = *pchar;
    }
  }
  match_string[match_string_len] = 0;
}

int main(void)
{
  startedin_c64 = (PEEK(0x0100) == 8);
  mega65_io_enable();

  POKE(0xd020,0);
  POKE(0xd021,0);  
  
  // Install NMI and BRK catchers
  POKE(0x0316,(uint8_t)(((uint16_t)&brk_catcher)>>0));
  POKE(0x0317,(uint8_t)(((uint16_t)&brk_catcher)>>8));
  POKE(0x0318,(uint8_t)(((uint16_t)&nmi_catcher)>>0));
  POKE(0x0319,(uint8_t)(((uint16_t)&nmi_catcher)>>8));
  
  c64_40col_mode();

  // Apply initial serial port settings
  modem_setup_serial(current_uart,baud_list[current_baud_rate].baud_divisor - 1);	
  print_text40(0,1,0x0e,0,"Core Boot 0.1");
  print_text40(0,2,0x0e,0,"Press any key to skip load.");
  if (impatient_sleep(1)) {
    universal_reset();
  }
  print_text40(0,2,0x0e,1,"");

  read_conf_file();
  parse_conf_file();

  POKE(0xD02fL, 0x47);
  POKE(0xd02fL, 0x53);

  print_text40(0, 4, 1, 1, "Searching for core:");
  print_text40(0, 5, 4, 1, match_string);

  wait_for_modem();
  read_cores("/");

  int c_idx = 0;
  for (c_idx = 0; c_idx < core_count; c_idx++) {

    if (partial_match(cores[c_idx].title, match_string)) {
      print_text40(0, 8, 1, 1, "Loading core:");
      print_text40(0, 9, 7, 1, cores[c_idx].title);

      impatient_sleep(5);

      char cmd[40];
      strcpy(cmd, "AT+JTAGLOAD=");
      strcat(cmd, cores[c_idx].number);
      send_cmd(cmd);
      while(1);  
    }
  }

  print_text40(0, 8, 2, 1, "Unable to locate requested core.");
  print_text40(0, 9, 2, 1, "Press any key or wait to exit.");
  impatient_sleep(25);
  universal_reset();
}
 
