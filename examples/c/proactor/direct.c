/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include <proton/condition.h>
#include <proton/connection.h>
#include <proton/delivery.h>
#include <proton/link.h>
#include <proton/listener.h>
#include <proton/message.h>
#include <proton/proactor.h>
#include <proton/sasl.h>
#include <proton/session.h>
#include <proton/transport.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct app_data_t {
  const char *host, *port;
  const char *amqp_address;
  const char *container_id;
  int message_count;

  pn_proactor_t *proactor;
  pn_listener_t *listener;
  pn_rwbytes_t message_buffer;

  /* Sender values */
  int sent;
  int acknowledged;
  pn_link_t *sender;

  /* Receiver values */
  int received;
} app_data_t;

static const int BATCH = 1000; /* Batch size for unlimited receive */

static int exit_code = 0;

static void check_condition(pn_event_t *e, pn_condition_t *cond) {
  if (pn_condition_is_set(cond)) {
    fprintf(stderr, "%s: %s: %s\n", pn_event_type_name(pn_event_type(e)),
            pn_condition_get_name(cond), pn_condition_get_description(cond));
    pn_connection_close(pn_event_connection(e));
    exit_code = 1;
  }
}

/* Create a message with a map { "sequence" : number } encode it and return the encoded buffer. */
static pn_bytes_t encode_message(app_data_t* app) {
  /* Construct a message with the map { "sequence": app.sent } */
  pn_message_t* message = pn_message();
  pn_data_put_int(pn_message_id(message), app->sent); /* Set the message_id also */
  pn_data_t* body = pn_message_body(message);
  pn_data_put_map(body);
  pn_data_enter(body);
  pn_data_put_string(body, pn_bytes(sizeof("sequence")-1, "sequence"));
  pn_data_put_int(body, app->sent); /* The sequence number */
  pn_data_exit(body);

  /* encode the message, expanding the encode buffer as needed */
  if (app->message_buffer.start == NULL) {
    static const size_t initial_size = 128;
    app->message_buffer = pn_rwbytes(initial_size, (char*)malloc(initial_size));
  }
  /* app->message_buffer is the total buffer space available. */
  /* mbuf wil point at just the portion used by the encoded message */
  pn_rwbytes_t mbuf = pn_rwbytes(app->message_buffer.size, app->message_buffer.start);
  int status = 0;
  while ((status = pn_message_encode(message, mbuf.start, &mbuf.size)) == PN_OVERFLOW) {
    app->message_buffer.size *= 2;
    app->message_buffer.start = (char*)realloc(app->message_buffer.start, app->message_buffer.size);
    mbuf.size = app->message_buffer.size;
  }
  if (status != 0) {
    fprintf(stderr, "error encoding message: %s\n", pn_error_text(pn_message_error(message)));
    exit(1);
  }
  pn_message_free(message);
  return pn_bytes(mbuf.size, mbuf.start);
}

#define MAX_SIZE 1024

static void decode_message(pn_delivery_t *dlv) {
  static char buffer[MAX_SIZE];
  ssize_t len;
  // try to decode the message body
  if (pn_delivery_pending(dlv) < MAX_SIZE) {
    // read in the raw data
    len = pn_link_recv(pn_delivery_link(dlv), buffer, MAX_SIZE);
    if (len > 0) {
      // decode it into a proton message
      pn_message_t *m = pn_message();
      if (PN_OK == pn_message_decode(m, buffer, len)) {
        pn_string_t *s = pn_string(NULL);
        pn_inspect(pn_message_body(m), s);
        printf("%s\n", pn_string_get(s));
        pn_free(s);
      }
      pn_message_free(m);
    }
  }
}

/* This function handles events when we are acting as the receiver */
static void handle_receive(app_data_t* app, pn_event_t* event) {
  switch (pn_event_type(event)) {

   case PN_LINK_REMOTE_OPEN: {
     pn_link_t *l = pn_event_link(event);
     pn_link_open(l);
     pn_link_flow(l, app->message_count ? app->message_count : BATCH);
   } break;

   case PN_DELIVERY: {
     /* A message has been received */
     pn_link_t *link = NULL;
     pn_delivery_t *dlv = pn_event_delivery(event);
     if (pn_delivery_readable(dlv) && !pn_delivery_partial(dlv)) {
       link = pn_delivery_link(dlv);
       decode_message(dlv);
       /* Accept the delivery */
       pn_delivery_update(dlv, PN_ACCEPTED);
       /* done with the delivery, move to the next and free it */
       pn_link_advance(link);
       pn_delivery_settle(dlv);  /* dlv is now freed */

       if (app->message_count == 0) {
         /* receive forever - see if more credit is needed */
         if (pn_link_credit(link) < BATCH/2) {
           /* Grant enough credit to bring it up to BATCH: */
           pn_link_flow(link, BATCH - pn_link_credit(link));
         }
       } else if (++app->received >= app->message_count) {
         /* done receiving, close the endpoints */
         printf("%d messages received\n", app->received);
         pn_session_t *ssn = pn_link_session(link);
         pn_link_close(link);
         pn_session_close(ssn);
         pn_connection_close(pn_session_connection(ssn));
       }
     }
   } break;

   default:
    break;
  }
}

/* This function handles events when we are acting as the sender */
static void handle_send(app_data_t* app, pn_event_t* event) {
  switch (pn_event_type(event)) {

   case PN_LINK_REMOTE_OPEN: {
     pn_link_t* l = pn_event_link(event);
     pn_terminus_set_address(pn_link_target(l), app->amqp_address);
     pn_link_open(l);
   } break;

   case PN_LINK_FLOW: {
     /* The peer has given us some credit, now we can send messages */
     pn_link_t *sender = pn_event_link(event);
     while (pn_link_credit(sender) > 0 && app->sent < app->message_count) {
       ++app->sent;
       // Use sent counter as unique delivery tag.
       pn_delivery(sender, pn_dtag((const char *)&app->sent, sizeof(app->sent)));
       pn_bytes_t msgbuf = encode_message(app);
       pn_link_send(sender, msgbuf.start, msgbuf.size);
       pn_link_advance(sender);
     }
     break;
   }

   case PN_DELIVERY: {
     /* We received acknowledgedment from the peer that a message was delivered. */
     pn_delivery_t* d = pn_event_delivery(event);
     if (pn_delivery_remote_state(d) == PN_ACCEPTED) {
       if (++app->acknowledged == app->message_count) {
         printf("%d messages sent and acknowledged\n", app->acknowledged);
         pn_connection_close(pn_event_connection(event));
         /* Continue handling events till we receive TRANSPORT_CLOSED */
       }
     }
   } break;

   default:
    break;
  }
}

/* Handle all events, delegate to handle_send or handle_receive depending on link mode.
   Return true to continue, false to exit
*/
static bool handle(app_data_t* app, pn_event_t* event) {
  switch (pn_event_type(event)) {

   case PN_LISTENER_OPEN:
    printf("listening\n");
    fflush(stdout);
    break;

   case PN_LISTENER_ACCEPT:
    pn_listener_accept(pn_event_listener(event), pn_connection());
    break;

   case PN_CONNECTION_INIT:
    pn_connection_set_container(pn_event_connection(event), app->container_id);
    break;

   case PN_CONNECTION_BOUND: {
     /* Turn off security */
     pn_transport_t *t = pn_event_transport(event);
     pn_transport_require_auth(t, false);
     pn_sasl_allowed_mechs(pn_sasl(t), "ANONYMOUS");
   }
   case PN_CONNECTION_REMOTE_OPEN: {
     pn_connection_open(pn_event_connection(event)); /* Complete the open */
     break;
   }

   case PN_SESSION_REMOTE_OPEN: {
     pn_session_open(pn_event_session(event));
     break;
   }

   case PN_TRANSPORT_CLOSED:
    check_condition(event, pn_transport_condition(pn_event_transport(event)));
    pn_listener_close(app->listener); /* Finished */
    break;

   case PN_CONNECTION_REMOTE_CLOSE:
    check_condition(event, pn_connection_remote_condition(pn_event_connection(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_SESSION_REMOTE_CLOSE:
    check_condition(event, pn_session_remote_condition(pn_event_session(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_LINK_REMOTE_CLOSE:
   case PN_LINK_REMOTE_DETACH:
    check_condition(event, pn_link_remote_condition(pn_event_link(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_PROACTOR_TIMEOUT:
    /* Wake the sender's connection */
    pn_connection_wake(pn_session_connection(pn_link_session(app->sender)));
    break;

   case PN_LISTENER_CLOSE:
    check_condition(event, pn_listener_condition(pn_event_listener(event)));
    break;

   case PN_PROACTOR_INACTIVE:
    return false;
    break;

   default: {
     pn_link_t *l = pn_event_link(event);
     if (l) {                      /* Only delegate link-related events */
       if (pn_link_is_sender(l)) {
         handle_send(app, event);
       } else {
         handle_receive(app, event);
       }
     }
   }
  }
  return true;
}

void run(app_data_t *app) {
  /* Loop and handle events */
  do {
    pn_event_batch_t *events = pn_proactor_wait(app->proactor);
    for (pn_event_t *e = pn_event_batch_next(events); e; e = pn_event_batch_next(events)) {
      if (!handle(app, e)) {
        return;
      }
    }
    pn_proactor_done(app->proactor, events);
  } while(true);
}

int main(int argc, char **argv) {
  struct app_data_t app = {0};
  int i = 0;
  app.container_id = argv[i++];   /* Should be unique */
  app.host = (argc > 1) ? argv[i++] : "";
  app.port = (argc > 1) ? argv[i++] : "amqp";
  app.amqp_address = (argc > i) ? argv[i++] : "example";
  app.message_count = (argc > i) ? atoi(argv[i++]) : 10;

  /* Create the proactor and connect */
  app.proactor = pn_proactor();
  app.listener = pn_listener();
  char addr[PN_MAX_ADDR];
  pn_proactor_addr(addr, sizeof(addr), app.host, app.port);
  pn_proactor_listen(app.proactor, app.listener, addr, 16);
  run(&app);
  pn_proactor_free(app.proactor);
  free(app.message_buffer.start);
  return exit_code;
}
