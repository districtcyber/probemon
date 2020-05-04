#include <stdio.h>
#include <stdlib.h>
#include <pcap/pcap.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sqlite3.h>
#include <time.h>

#include "queue.h"
#include "parsers.h"
#include "logger_thread.h"
#include "db.h"
#include "manuf.h"
#include "config_yaml.h"

#define NAME "probemon"
#define VERSION "0.1"

#define SNAP_LEN 512
#define MAX_QUEUE_SIZE 128

#define DB_NAME "./probemon.db"
#define MANUF_NAME "./manuf"
#define CONFIG_NAME "./config.yaml"

pcap_t *handle;                 // global, to use it in sigint_handler
queue_t *queue;                 // queue to hold parsed ap infos

pthread_t logger;
pthread_mutex_t mutex_queue = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
struct timespec start_ts_queue;
bool option_stdout;

sqlite3 *db = NULL;
int ret = 0;

size_t ouidb_size;
manuf_t *ouidb;
uint64_t *ignored = NULL;

void sigint_handler(int s)
{
  // stop pcap capture loop
  pcap_breakloop(handle);
}

void lower(char *p)
{
  // convert string (in place) to lowercase
  for ( ; *p; ++p) *p = tolower(*p);
}

void process_packet(uint8_t * args, const struct pcap_pkthdr *header, const uint8_t *packet)
{
  uint16_t freq;
  int8_t rssi;
  // parse radiotap header
  int8_t offset = parse_radiotap_header(packet, &freq, &rssi);
  if (offset < 0) {
    return;
  }

  char *mac;
  uint8_t ssid_len, *ssid;

  parse_probereq_frame(packet, header->len, offset, &mac, &ssid, &ssid_len);
  lower(mac);

  probereq_t *pr = malloc(sizeof(probereq_t));
  pr->ts = header->ts.tv_sec;
  pr->mac = mac;
  pr->ssid = ssid;
  pr->ssid_len = ssid_len;
  pr->vendor = NULL;
  pr->rssi = rssi;

  pthread_mutex_lock(&mutex_queue);
  enqueue(queue, pr);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (queue->size == MAX_QUEUE_SIZE / 2 || now.tv_sec - start_ts_queue.tv_sec >= 1) {
    start_ts_queue = now;
    // the queue is half full or it's been more than a second; waking up the logger thread to process that
    pthread_cond_signal(&cv);
  }
  pthread_mutex_unlock(&mutex_queue);
}

void usage(void)
{
  printf("Usage: probemon -i IFACE -c CHANNEL [-d DB_NAME] [-m MANUF_NAME] [-s]\n");
  printf("  -i IFACE        interface to use\n"
         "  -c CHANNEL      channel to sniff on\n"
         "  -d DB_NAME      explicitly set the db filename\n"
         "  -m MANUF_NAME   path to manuf file\n"
         "  -s              also log probe requests to stdout\n"
       );
}

int main(int argc, char *argv[])
{
  char *iface = NULL;
  char *option_channel = NULL;
  char *option_db_name = NULL;
  char *option_manuf_name = NULL;
  char *db_name = NULL;
  char *manuf_name = NULL;
  int opt;
  uint8_t channel;

  option_stdout = false;
  while ((opt = getopt(argc, argv, "c:hi:d:m:sV")) != -1) {
    switch (opt) {
    case 'h':
      usage();
      exit(EXIT_SUCCESS);
      break;
    case 'i':
      iface = optarg;
      break;
    case 'c':
      option_channel = optarg;
      break;
    case 'd':
      option_db_name = optarg;
      break;
    case 'm':
      option_manuf_name = optarg;
      break;
    case 's':
      option_stdout = true;
      break;
    case 'V':
      printf("%s %s\nCopyright © 2020 solsTice d'Hiver\nLicense GPLv3+: GNU GPL version 3\n", NAME, VERSION);
      exit(EXIT_SUCCESS);
      break;
    case '?':
      usage();
      exit(EXIT_FAILURE);
    default:
      usage();
      exit(EXIT_FAILURE);
    }
  }

  if (iface == NULL) {
    fprintf(stderr, "Error: no interface selected\n");
    exit(EXIT_FAILURE);
  }
  //printf("The device you entered: %s\n", iface);

  if (option_channel == NULL) {
    fprintf(stderr, "Error: no channel defined\n");
    exit(EXIT_FAILURE);
  } else {
    channel = (uint8_t)strtol(option_channel, NULL, 10);
  }

  if (option_manuf_name == NULL) {
    manuf_name = strdup(MANUF_NAME);
  } else {
    manuf_name = strdup(option_manuf_name);
  }
  // look for manuf file and parse it into memory
  if (access(manuf_name, F_OK) == -1 ) {
    fprintf(stderr, "Error: can't find manuf file %s\n", manuf_name);
    exit(EXIT_FAILURE);
  }
  printf(":: Parsing manuf file...\n");
  fflush(stdout);
  ouidb = parse_manuf_file(manuf_name, &ouidb_size);
  if (ouidb == NULL) {
    fprintf(stderr, "Error: can't parse manuf file\n");
    exit(EXIT_FAILURE);
  }

  int ignored_count = 0;
  char **entries = parse_config_yaml(CONFIG_NAME, "ignored", &ignored_count);
  ignored = parse_ignored_entries(entries, ignored_count);
  for (int i=0; i<ignored_count; i++) {
    free(entries[i]);
  }
  free(entries);

  char errbuf[PCAP_ERRBUF_SIZE];
  // just check if iface is in the list of known devices
  pcap_if_t *devs = NULL;
  if (pcap_findalldevs(&devs, errbuf) == 0) {
    pcap_if_t *d = devs;
    bool found = false;
    while (!found && d != NULL) {
      if ((strlen(d->name) == strlen(iface))
          && (memcmp(d->name, iface, strlen(iface)) == 0)) {
        found = true;
        break;
      }
      d = d->next;
    }
    pcap_freealldevs(devs);
    if (!found) {
      fprintf(stderr, "Error: %s is not a known interface.\n", iface);
      exit(EXIT_FAILURE);
    }
  }

  // look up for iw in system
  char *iw;
  if (access("/sbin/iw", F_OK) != -1) {
    iw = strdup("/sbin/iw");
  } else if (access("/usr/sbin/iw", F_OK) != -1) {
    iw = strdup("/usr/sbin/iw");
  } else if (access("/usr/bin/iw", F_OK) != -1) {
    iw = strdup("/usr/bin/iw");
  } else {
    fprintf(stderr, "Error: couldn't find iw on system (in /sbin, /usr/sbin, /usr/bin)\n");
    exit(EXIT_FAILURE);
  }
  // change the channel to listen on
  char cmd[128];
  snprintf(cmd, 128, "%s dev %s set channel %d", iw, iface, channel);
  free(iw);
  if (system(cmd)) {
    fprintf(stderr, "Error: can't change to channel %d with iw on interface %s\n", channel, iface);
    exit(EXIT_FAILURE);
  }

  handle = pcap_create(iface, errbuf);
  if (handle == NULL) {
    fprintf(stderr, "Error: unable to create pcap handle: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
  pcap_set_snaplen(handle, SNAP_LEN);
  pcap_set_timeout(handle, 1000);
  pcap_set_promisc(handle, 1);

  if (pcap_activate(handle)) {
    pcap_perror(handle, "Error: ");
    exit(EXIT_FAILURE);
  }
  // only capture packets received by interface
  if (pcap_setdirection(handle, PCAP_D_IN)) {
    pcap_perror(handle, "Error: ");
    exit(EXIT_FAILURE);
  }

  int *dlt_buf, dlt_buf_len;
  dlt_buf_len = pcap_list_datalinks(handle, &dlt_buf);
  if (dlt_buf_len < 0) {
    pcap_perror(handle, "Error: ");
    exit(EXIT_FAILURE);
  }
  bool found = false;
  for (int i=0; i< dlt_buf_len; i++) {
    if  (dlt_buf[i] == DLT_IEEE802_11_RADIO) {
      found = true;
    }
  }
  pcap_free_datalinks(dlt_buf);

  if (found) {
    if (pcap_set_datalink(handle, DLT_IEEE802_11_RADIO)) {
      pcap_perror(handle, "Error: ");
      exit(EXIT_FAILURE);
    }
  } else {
    fprintf(stderr, "Error: the interface %s does not support radiotap header or is not in monitor mode\n", iface);
    exit(EXIT_FAILURE);
  }

  // only capture probe request frames
  struct bpf_program bfp;
  char filter_exp[] = "type mgt subtype probereq";

  if (pcap_compile(handle, &bfp, filter_exp, 1, PCAP_NETMASK_UNKNOWN) == -1) {
    fprintf(stderr, "Error: couldn't parse filter %s: %s\n",
            filter_exp, pcap_geterr(handle));
    ret = EXIT_FAILURE;
    goto handle_failure;
  }
  if (pcap_setfilter(handle, &bfp) == -1) {
    fprintf(stderr, "Error: couldn't install filter '%s': %s\n",
            filter_exp, pcap_geterr(handle));
    exit(EXIT_FAILURE);
  }
  pcap_freecode(&bfp);

  queue = new_queue(MAX_QUEUE_SIZE);
  // start the helper logger thread
  if (pthread_create(&logger, NULL, process_queue, NULL)) {
    fprintf(stderr, "Error creating logger thread\n");
    ret = EXIT_FAILURE;
    goto logger_failure;
  }
  pthread_detach(logger);

  struct sigaction act;
  act.sa_handler = sigint_handler;
  act.sa_flags = 0;
  sigemptyset(&act.sa_mask);
  // catch CTRL+C to break loop cleanly
  sigaction(SIGINT, &act, NULL);
  // catch quit signal to flush data to file on disk
  sigaction(SIGQUIT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);

  clock_gettime(CLOCK_MONOTONIC, &start_ts_queue);

  if (option_db_name == NULL) {
    db_name = strdup(DB_NAME);
  } else {
    db_name = strdup(option_db_name);
  }

  if (init_probemon_db(db_name, &db) != 0) {
    fprintf(stderr, "Error: can't initialize sqlite3 db %s\n", db_name);
    goto logger_failure;
  }
  begin_txn(db);

  // we have to cheat a little and print the message before pcap_loop
  printf(":: Started sniffing probe requests with %s on channel %d, writing to %s\n", iface, channel, db_name);
  printf("Hit CTRL+C to quit\n");
  fflush(stdout);

  int err;
  if ((err = pcap_loop(handle, -1, (pcap_handler) process_packet, NULL))) {
    if (err == PCAP_ERROR) {
      pcap_perror(handle, "Error: ");
    }
    if (err == PCAP_ERROR_BREAK) {
      printf("exiting...\n");
    }
  }

  commit_txn(db);
  sqlite3_close(db);

  // free up manuf table
  for (int i=0; i<ouidb_size; i++) {
    free(ouidb[i].short_oui);
    free(ouidb[i].long_oui);
    free(ouidb[i].comment);
  }
  free(ouidb);

logger_failure:
  pthread_cancel(logger);

  // free up elements of the queue
  int qs = queue->size;
  probereq_t *pr;
  for (int i = 0; i < qs; i++) {
    pr = (probereq_t *) dequeue(queue);
    free_probereq(pr);
  }
  free(queue);

  pthread_mutex_destroy(&mutex_queue);
  pthread_cond_destroy(&cv);

handle_failure:
  pcap_close(handle);

  free(db_name);

  return ret;
}
