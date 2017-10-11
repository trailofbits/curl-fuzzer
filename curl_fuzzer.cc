/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 2017, Max Dymond, <cmeister2@gmail.com>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "curl_fuzzer.h"

/**
 * Fuzzing entry point. This function is passed a buffer containing a test
 * case.  This test case should drive the CURL API into making a request.
 */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  int rc = 0;
  int tlv_rc;
  FUZZ_DATA fuzz;
  TLV tlv;

  /* Have to set all fields to zero before getting to the terminate function */
  memset(&fuzz, 0, sizeof(FUZZ_DATA));

  if(size < sizeof(TLV_RAW)) {
    /* Not enough data for a single TLV - don't continue */
    goto EXIT_LABEL;
  }

  /* Try to initialize the fuzz data */
  FTRY(fuzz_initialize_fuzz_data(&fuzz, data, size));

  for(tlv_rc = fuzz_get_first_tlv(&fuzz, &tlv);
      tlv_rc == 0;
      tlv_rc = fuzz_get_next_tlv(&fuzz, &tlv)) {

    /* Have the TLV in hand. Parse the TLV. */
    rc = fuzz_parse_tlv(&fuzz, &tlv);

    if(rc != 0) {
      /* Failed to parse the TLV. Can't continue. */
      goto EXIT_LABEL;
    }
  }

  if(tlv_rc != TLV_RC_NO_MORE_TLVS) {
    /* A TLV call failed. Can't continue. */
    goto EXIT_LABEL;
  }

  /* Set up the standard easy options. */
  FTRY(fuzz_set_easy_options(&fuzz));

  /**
   * Add in more curl options that have been accumulated over possibly
   * multiple TLVs.
   */
  if(fuzz.header_list != NULL) {
    curl_easy_setopt(fuzz.easy, CURLOPT_HTTPHEADER, fuzz.header_list);
  }

  if(fuzz.mail_recipients_list != NULL) {
    curl_easy_setopt(fuzz.easy, CURLOPT_MAIL_RCPT, fuzz.mail_recipients_list);
  }

  if(fuzz.mime != NULL) {
    curl_easy_setopt(fuzz.easy, CURLOPT_MIMEPOST, fuzz.mime);
  }

  /* Run the transfer. */
  fuzz_handle_transfer(&fuzz);

EXIT_LABEL:

  fuzz_terminate_fuzz_data(&fuzz);

  /* This function must always return 0. Non-zero codes are reserved. */
  return 0;
}

/**
 * Utility function to convert 4 bytes to a u32 predictably.
 */
uint32_t to_u32(const uint8_t b[4])
{
  uint32_t u;
  u = (b[0] << 24) + (b[1] << 16) + (b[2] << 8) + b[3];
  return u;
}

/**
 * Utility function to convert 2 bytes to a u16 predictably.
 */
uint16_t to_u16(const uint8_t b[2])
{
  uint16_t u;
  u = (b[0] << 8) + b[1];
  return u;
}

/**
 * Initialize the local fuzz data structure.
 */
int fuzz_initialize_fuzz_data(FUZZ_DATA *fuzz,
                              const uint8_t *data,
                              size_t data_len)
{
  int rc = 0;

  /* Initialize the fuzz data. */
  memset(fuzz, 0, sizeof(FUZZ_DATA));

  /* Create an easy handle. This will have all of the settings configured on
     it. */
  fuzz->easy = curl_easy_init();
  FCHECK(fuzz->easy != NULL);

  /* Set up the state parser */
  fuzz->state.data = data;
  fuzz->state.data_len = data_len;

  /* Set up the state of the server socket. */
  fuzz->server_fd_state = FUZZ_SOCK_CLOSED;

EXIT_LABEL:

  return rc;
}

/**
 * Set standard options on the curl easy.
 */
int fuzz_set_easy_options(FUZZ_DATA *fuzz)
{
  int rc = 0;

  /* Set some standard options on the CURL easy handle. We need to override the
     socket function so that we create our own sockets to present to CURL. */
  FTRY(curl_easy_setopt(fuzz->easy,
                        CURLOPT_OPENSOCKETFUNCTION,
                        fuzz_open_socket));
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_OPENSOCKETDATA, fuzz));

  /* In case something tries to set a socket option, intercept this. */
  FTRY(curl_easy_setopt(fuzz->easy,
                        CURLOPT_SOCKOPTFUNCTION,
                        fuzz_sockopt_callback));

  /* Set the standard read function callback. */
  FTRY(curl_easy_setopt(fuzz->easy,
                        CURLOPT_READFUNCTION,
                        fuzz_read_callback));
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_READDATA, fuzz));

  /* Set the standard write function callback. */
  FTRY(curl_easy_setopt(fuzz->easy,
                        CURLOPT_WRITEFUNCTION,
                        fuzz_write_callback));
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_WRITEDATA, fuzz));

  /* Set the cookie jar so cookies are tested. */
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_COOKIEJAR, FUZZ_COOKIE_JAR_PATH));

  /* Time out requests quickly. */
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_TIMEOUT_MS, 200L));

  /* Can enable verbose mode by having the environment variable FUZZ_VERBOSE. */
  if (getenv("FUZZ_VERBOSE") != NULL)
  {
    FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_VERBOSE, 1L));
  }

  fuzz->connect_to_list = curl_slist_append(NULL, "::127.0.1.127:");
  FTRY(curl_easy_setopt(fuzz->easy, CURLOPT_CONNECT_TO, fuzz->connect_to_list));

EXIT_LABEL:

  return rc;
}

/**
 * Terminate the fuzz data structure, including freeing any allocated memory.
 */
void fuzz_terminate_fuzz_data(FUZZ_DATA *fuzz)
{
  fuzz_free((void **)&fuzz->postfields);

  if(fuzz->server_fd_state != FUZZ_SOCK_CLOSED){
    close(fuzz->server_fd);
    fuzz->server_fd_state = FUZZ_SOCK_CLOSED;
  }

  if(fuzz->connect_to_list != NULL) {
    curl_slist_free_all(fuzz->connect_to_list);
    fuzz->connect_to_list = NULL;
  }

  if(fuzz->header_list != NULL) {
    curl_slist_free_all(fuzz->header_list);
    fuzz->header_list = NULL;
  }

  if(fuzz->mail_recipients_list != NULL) {
    curl_slist_free_all(fuzz->mail_recipients_list);
    fuzz->mail_recipients_list = NULL;
  }

  if(fuzz->mime != NULL) {
    curl_mime_free(fuzz->mime);
    fuzz->mime = NULL;
  }

  if(fuzz->easy != NULL) {
    curl_easy_cleanup(fuzz->easy);
    fuzz->easy = NULL;
  }
}

/**
 * If a pointer has been allocated, free that pointer.
 */
void fuzz_free(void **ptr)
{
  if(*ptr != NULL) {
    free(*ptr);
    *ptr = NULL;
  }
}

/**
 * Function for handling the fuzz transfer, including sending responses to
 * requests.
 */
int fuzz_handle_transfer(FUZZ_DATA *fuzz)
{
  int rc = 0;
  CURLM *multi_handle;
  int still_running; /* keep number of running handles */
  CURLMsg *msg; /* for picking up messages with the transfer status */
  int msgs_left; /* how many messages are left */
  int double_timeout = 0;
  fd_set fdread;
  fd_set fdwrite;
  fd_set fdexcep;
  struct timeval timeout;
  int select_rc;
  CURLMcode mc;
  int maxfd = -1;
  long curl_timeo = -1;

  /* Set up the starting index for responses. */
  fuzz->response_index = 1;

  /* init a multi stack */
  multi_handle = curl_multi_init();

  /* add the individual transfers */
  curl_multi_add_handle(multi_handle, fuzz->easy);

  do {
    /* Reset the sets of file descriptors. */
    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);

    /* Set a timeout of 10ms. This is lower than recommended by the multi guide
       but we're not going to any remote servers, so everything should complete
       very quickly. */
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;

    /* get file descriptors from the transfers */
    mc = curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
    if(mc != CURLM_OK) {
      fprintf(stderr, "curl_multi_fdset() failed, code %d.\n", mc);
      rc = -1;
      break;
    }

    /* Add the socket FD into the readable set if connected. */
    if(fuzz->server_fd_state == FUZZ_SOCK_OPEN) {
      FD_SET(fuzz->server_fd, &fdread);

      /* Work out the maximum FD between the cURL file descriptors and the
         server FD. */
      maxfd = (fuzz->server_fd > maxfd) ? fuzz->server_fd : maxfd;
    }

    /* Work out what file descriptors need work. */
    rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);

    if(rc == -1) {
      /* Had an issue while selecting a file descriptor. Let's just exit. */
      break;
    }
    else if(rc == 0) {
      /* Timed out. */
      if(double_timeout == 1) {
        /* We don't expect multiple timeouts in a row. If there are double
           timeouts then exit. */
        break;
      }
      else {
        /* Set the timeout flag for the next time we select(). */
        double_timeout = 1;
      }
    }
    else {
      /* There's an active file descriptor. Reset the timeout flag. */
      double_timeout = 0;
    }

    /* Check to see if the server file descriptor is readable. If it is,
       then send the next response from the fuzzing data. */
    if(fuzz->server_fd_state == FUZZ_SOCK_OPEN &&
       FD_ISSET(fuzz->server_fd, &fdread)) {
      rc = fuzz_send_next_response(fuzz);
      if(rc != 0) {
        /* Failed to send a response. Break out here. */
        break;
      }
    }

    /* Process the multi object. */
    curl_multi_perform(multi_handle, &still_running);

  } while(still_running);

  /* Remove the easy handle from the multi stack. */
  curl_multi_remove_handle(multi_handle, fuzz->easy);

  /* Clean up the multi handle - the top level function will handle the easy
     handle. */
  curl_multi_cleanup(multi_handle);

  return(rc);
}

/**
 * Sends the next fuzzing response to the server file descriptor.
 */
int fuzz_send_next_response(FUZZ_DATA *fuzz)
{
  int rc = 0;
  ssize_t ret_in;
  ssize_t ret_out;
  char buffer[8192];
  const uint8_t *data;
  size_t data_len;
  int is_verbose;

  /* Work out if we're tracing out. If we are, trace out the received data. */
  is_verbose = (getenv("FUZZ_VERBOSE") != NULL);

  /* Need to read all data sent by the client so the file descriptor becomes
     unreadable. Because the file descriptor is non-blocking we won't just
     hang here. */
  do {
    ret_in = read(fuzz->server_fd, buffer, sizeof(buffer));
    if(is_verbose && ret_in > 0) {
      printf("FUZZ: Received %zu bytes \n==>\n", ret_in);
      fwrite(buffer, ret_in, 1, stdout);
      printf("\n<==\n");
    }
  } while (ret_in > 0);

  /* Now send a response to the request that the client just made. */
  data = fuzz->responses[fuzz->response_index].data;
  data_len = fuzz->responses[fuzz->response_index].data_len;

  if(data != NULL) {
    if(write(fuzz->server_fd, data, data_len) != (ssize_t)data_len) {
      /* Failed to write the data back to the client. Prevent any further
         testing. */
      rc = -1;
    }
  }

  /* Work out if there are any more responses. If not, then shut down the
     server. */
  fuzz->response_index++;

  if(fuzz->response_index > TLV_MAX_NUM_RESPONSES ||
     fuzz->responses[fuzz->response_index].data == NULL) {
    shutdown(fuzz->server_fd, SHUT_WR);
    fuzz->server_fd_state = FUZZ_SOCK_SHUTDOWN;
  }

  return(rc);
}
