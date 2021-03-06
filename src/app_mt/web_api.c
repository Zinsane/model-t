
#include <ch.h>
#include <hal.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pb_encode.h>
#include <pb_decode.h>

#include "web_api.h"
#include "bbmt.pb.h"
#include "message.h"
#include "net.h"
#include "sensor.h"
#include "app_cfg.h"
#include "temp_control.h"
#include "app_cfg.h"
#include "ota_update.h"
#include "sxfs.h"
#include "pid.h"

#ifndef WEB_API_HOST
#define WEB_API_HOST_STR "dg.brewbit.com"
#else
#define xstr(s) str(s)
#define str(s) #s
#define WEB_API_HOST_STR xstr(WEB_API_HOST)
#endif

#ifndef WEB_API_PORT
#define WEB_API_PORT 31337
#endif

#define SENSOR_REPORT_INTERVAL S2ST(30)
#define SETTINGS_UPDATE_DELAY  S2ST(1 * 60)
#define MIN_SEND_INTERVAL      S2ST(10)
#define RECV_TIMEOUT           S2ST(20)
#define MAX_SEND_ERRS          25


typedef enum {
  RECV_LEN,
  RECV_DATA
} parser_state_t;

typedef struct {
  bool new_sample;
  bool new_settings;
  quantity_t last_sample;
} api_controller_status_t;

typedef struct {
  parser_state_t state;

  uint8_t* recv_buf;
  uint32_t bytes_remaining;

  uint32_t data_len;
  uint8_t data_buf[ApiMessage_size];
} msg_parser_t;

typedef struct {
  int socket;
  api_status_t status;

  bool server_time_available;
  time_t last_server_time;
  systime_t last_server_systime;

  bool new_device_settings;
  api_controller_status_t controller_status[NUM_SENSORS];
  systime_t last_sensor_report_time;
  systime_t last_send_time;
  systime_t last_recv_time;
  uint32_t send_errors;
  msg_parser_t parser;
  msg_listener_t* msg_listener;
  uint32_t backlog_pos;
} web_api_t;


static void
set_state(web_api_t* api, api_state_t state);

static void
web_api_dispatch(msg_id_t id, void* msg_data, void* listener_data, void* sub_data);

static void
web_api_idle(web_api_t* api);

static bool
was_authenticated(void);

static void
connect_to_server(web_api_t* api);

static void
monitor_connection(web_api_t* api);

static void
send_data_to_server(web_api_t* api);

static void
send_backlog(web_api_t* api);

static void
socket_message_rx(web_api_t* api, const uint8_t* data, uint32_t data_len);

static void
send_api_msg(web_api_t* api, ApiMessage* msg, bool can_backlog);

static void
dispatch_api_msg(web_api_t* api, ApiMessage* msg);

static void
dispatch_net_status(web_api_t* api, net_status_t* ns);

static void
dispatch_sensor_sample(web_api_t* api, sensor_msg_t* sample);

static void
dispatch_device_settings_from_device(
    web_api_t* api,
    output_ctrl_t* control_mode);

static void
dispatch_controller_settings_from_device(web_api_t* api,
    controller_settings_t* controller_settings_msg);

static void
send_device_settings(
    web_api_t* api);

static void
send_controller_settings(
    web_api_t* api);

static time_t
get_server_time(web_api_t* api);

static void
request_activation_token(web_api_t* api);

static void
request_auth(web_api_t* api);

static void
check_for_update(web_api_t* api);

static void
dispatch_firmware_rqst(web_api_t* api, firmware_update_t* firmware_data);

static void
send_sensor_report(web_api_t* api);

static void
dispatch_device_settings_from_server(DeviceSettings* settings);

static void
dispatch_controller_settings_from_server(ControllerSettings* settings);

static void
dispatch_server_time(web_api_t* api, ServerTime* server_time);

static bool
socket_connect(web_api_t* api, const char* hostname, uint16_t port);

static void
socket_poll(web_api_t* api);

static bool
send_or_store(web_api_t* api, void* buf, uint32_t buf_len, bool can_backlog);

static bool
socket_send(web_api_t* api, void* buf, uint32_t buf_len);


extern char device_id[32];
static web_api_t* api;


void
web_api_init()
{
  api = calloc(1, sizeof(web_api_t));
  api->status.state = AS_AWAITING_NET_CONNECTION;

  api->backlog_pos = 0;
  while (1) {
    uint32_t msg_len = 0xFFFFFFFF;
    bool ret = sxfs_read(SP_WEB_API_BACKLOG, api->backlog_pos, (uint8_t*)&msg_len, sizeof(msg_len));
    if (!ret || msg_len == 0xFFFFFFFF)
      break;

    api->backlog_pos += msg_len;
  }

  api->msg_listener = msg_listener_create("web_api", 2048, web_api_dispatch, api);
  msg_listener_set_idle_timeout(api->msg_listener, 100);
  msg_listener_enable_watchdog(api->msg_listener, 3 * 60 * 1000);

  msg_subscribe(api->msg_listener, MSG_NET_STATUS, NULL);
  msg_subscribe(api->msg_listener, MSG_API_FW_UPDATE_CHECK, NULL);
  msg_subscribe(api->msg_listener, MSG_API_FW_DNLD_RQST, NULL);
  msg_subscribe(api->msg_listener, MSG_SENSOR_SAMPLE, NULL);
  msg_subscribe(api->msg_listener, MSG_CONTROLLER_SETTINGS, NULL);
}

const api_status_t*
web_api_get_status()
{
  return &api->status;
}

const char*
web_api_get_endpoint()
{
  return WEB_API_HOST_STR;
}

static void
web_api_dispatch(msg_id_t id, void* msg_data, void* listener_data, void* sub_data)
{
  (void)sub_data;
  (void)msg_data;

  web_api_t* api = listener_data;

  switch (id) {
    case MSG_NET_STATUS:
      dispatch_net_status(api, msg_data);
      break;

    case MSG_SENSOR_SAMPLE:
      dispatch_sensor_sample(api, msg_data);
      break;

    case MSG_IDLE:
      web_api_idle(api);
      break;

    default:
      break;
  }

  // Only process the following if the API connection has been established
  if (api->status.state == AS_CONNECTED) {
    switch (id) {
      case MSG_API_FW_UPDATE_CHECK:
        check_for_update(api);
        break;

      case MSG_API_FW_DNLD_RQST:
        dispatch_firmware_rqst(api, msg_data);
        break;

      case MSG_CONTROL_MODE:
        dispatch_device_settings_from_device(api, msg_data);
        break;

      case MSG_CONTROLLER_SETTINGS:
        dispatch_controller_settings_from_device(api, msg_data);
        break;

      default:
        break;
    }
  }
}

static void
set_state(web_api_t* api, api_state_t state)
{
  if (api->status.state != state) {
    api->status.state = state;

    api_status_t status_msg = {
        .state = state
    };
    msg_send(MSG_API_STATUS, &status_msg);
  }
}

static void
web_api_idle(web_api_t* api)
{
  if (api->status.state == AS_CONNECTING)
    connect_to_server(api);
  else if (api->status.state > AS_CONNECTING) {
    monitor_connection(api);
    socket_poll(api);
  }

  send_data_to_server(api);
}

static bool
was_authenticated()
{
  const char* auth_token = app_cfg_get_auth_token();
  return auth_token[0] != 0;
}

static void
connect_to_server(web_api_t* api)
{
  printf("Connecting to: %s:%d\r\n", WEB_API_HOST_STR, WEB_API_PORT);
  if (socket_connect(api, WEB_API_HOST_STR, WEB_API_PORT)) {
    api->last_recv_time = chTimeNow();
    api->send_errors = 0;

    api->parser.state = RECV_LEN;
    api->parser.bytes_remaining = 4;
    api->parser.recv_buf = (uint8_t*)&api->parser.data_len;

    if (was_authenticated()) {
      set_state(api, AS_REQUESTING_AUTH);
      request_auth(api);
    }
    else {
      set_state(api, AS_REQUESTING_ACTIVATION_TOKEN);
      request_activation_token(api);
    }
  }
}

static void
monitor_connection(web_api_t* api)
{
  /* If we haven't heard from the server in a while, disconnect and try again */
  if ((chTimeNow() - api->last_recv_time) > RECV_TIMEOUT) {
    printf("Server timed out\r\n");
    closesocket(api->socket);
    api->socket = -1;
    set_state(api, AS_CONNECTING);
    return;
  }

  /* If we haven't sent anything to the server in a while, send a keepalive */
  if ((chTimeNow() - api->last_send_time) > MIN_SEND_INTERVAL) {
    uint32_t keepalive = 0;
    socket_send(api, &keepalive, 4);
  }
}

static void
send_data_to_server(web_api_t* api)
{
  if ((api->status.state == AS_CONNECTED) &&
      (api->backlog_pos > 0))
    send_backlog(api);

  if (was_authenticated()) {
    if ((chTimeNow() - api->last_sensor_report_time) > SENSOR_REPORT_INTERVAL) {
      send_sensor_report(api);
      api->last_sensor_report_time = chTimeNow();
    }

    if (api->new_device_settings) {
      send_device_settings(api);
      api->new_device_settings = false;
    }

    send_controller_settings(api);
  }
}

static void
send_backlog(web_api_t* api)
{
  uint32_t backlog_read_pos = 0;
  uint32_t data_left_to_send = api->backlog_pos;
  uint8_t* send_buf = malloc(1024);

  printf("Sending %d bytes from backlog\r\n", (int)data_left_to_send);

  while (data_left_to_send > 0) {
    uint32_t send_len = MIN(1024, data_left_to_send);
    if (!sxfs_read(SP_WEB_API_BACKLOG, backlog_read_pos, send_buf, send_len)) {
      printf("Backlog read failed!\r\n");
      break;
    }

    if (!socket_send(api, send_buf, send_len)) {
      printf("Backlog send failed!\r\n");
      break;
    }

    backlog_read_pos += send_len;
    data_left_to_send -= send_len;
  }
  free(send_buf);

  sxfs_erase_all(SP_WEB_API_BACKLOG);
  api->backlog_pos = 0;
}

static bool
socket_connect(web_api_t* api, const char* hostname, uint16_t port)
{
  uint32_t hostaddr;
  int ret = gethostbyname(hostname, strlen(hostname), &hostaddr);
  if (ret < 0) {
    printf("gethostbyname failed %d\r\n", ret);
    return false;
  }

  api->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (api->socket < 0) {
    printf("Connect failed %d\r\n", api->socket);
    return false;
  }

  int optval = SOCK_ON;
  ret = setsockopt(api->socket, SOL_SOCKET, SOCKOPT_RECV_NONBLOCK, (char *)&optval, sizeof(optval));
  if (ret < 0) {
    closesocket(api->socket);
    api->socket = -1;
    printf("setsockopt failed %d\r\n", ret);
    return false;
  }

  sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr.s_addr = htonl(hostaddr)
  };
  ret = connect(api->socket, (sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    closesocket(api->socket);
    api->socket = -1;
    printf("connect failed %d\r\n", ret);
    return false;
  }

  return true;
}

static void
socket_poll(web_api_t* api)
{
  int ret = recv(api->socket, api->parser.recv_buf, api->parser.bytes_remaining, 0);
  if (ret < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      printf("recv failed %d %d\r\n", ret, errno);
      printf("socket disconnected\r\n");
      closesocket(api->socket);
      api->socket = -1;
      set_state(api, AS_CONNECTING);
    }
  }
  else {
    api->parser.bytes_remaining -= ret;
    api->parser.recv_buf += ret;
    if (ret > 0)
      api->last_recv_time = chTimeNow();

    if (api->parser.bytes_remaining == 0) {
      switch (api->parser.state) {
        case RECV_LEN:
          api->parser.data_len = ntohl(api->parser.data_len);
          if (api->parser.data_len > 0) {
            api->parser.state = RECV_DATA;
            api->parser.bytes_remaining = api->parser.data_len;
            api->parser.recv_buf = api->parser.data_buf;
          }
          else {
            api->parser.state = RECV_LEN;
            api->parser.bytes_remaining = 4;
            api->parser.recv_buf = (uint8_t*)&api->parser.data_len;
          }
          break;

        case RECV_DATA:
          api->parser.bytes_remaining = 4;
          api->parser.recv_buf = (uint8_t*)&api->parser.data_len;
          api->parser.state = RECV_LEN;

          socket_message_rx(api, api->parser.data_buf, api->parser.data_len);
          break;
      }
    }
  }
}

static void
populate_output_status(ControllerReport* pr, sensor_id_t controller, output_id_t output)
{
  const controller_settings_t* controller_settings = app_cfg_get_controller_settings(controller);

  if (controller_settings->output_settings[output].enabled) {
    output_ctrl_t control_mode = app_cfg_get_control_mode();
    temp_control_status_t output_status = temp_control_get_status(controller, output);

    pr->output_status[pr->output_status_count].output_index = output;
    pr->output_status[pr->output_status_count].has_output_index = true;

    pr->output_status[pr->output_status_count].status = output_status.output_enabled;
    pr->output_status[pr->output_status_count].has_status = true;

    if (control_mode == PID) {
      pr->output_status[pr->output_status_count].kp = output_status.kp;
      pr->output_status[pr->output_status_count].has_kp = true;

      pr->output_status[pr->output_status_count].ki = output_status.ki;
      pr->output_status[pr->output_status_count].has_ki = true;

      pr->output_status[pr->output_status_count].kd = output_status.kd;
      pr->output_status[pr->output_status_count].has_kd = true;
    }
    pr->output_status_count++;
  }
}

static void
send_sensor_report(web_api_t* api)
{
  int i;
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_DEVICE_REPORT;
  msg->has_deviceReport = true;
  msg->deviceReport.controller_reports_count = 0;

  for (i = 0; i < NUM_SENSORS; ++i) {
    if (api->controller_status[i].new_sample) {
      api->controller_status[i].new_sample = false;
      ControllerReport* pr = &msg->deviceReport.controller_reports[msg->deviceReport.controller_reports_count];
      msg->deviceReport.controller_reports_count++;

      pr->controller_index = i;
      pr->sensor_reading = api->controller_status[i].last_sample.value;
      pr->setpoint = temp_control_get_current_setpoint(i);

      populate_output_status(pr, i, OUTPUT_1);
      populate_output_status(pr, i, OUTPUT_2);

      if (api->server_time_available) {
        pr->has_timestamp = true;
        pr->timestamp = get_server_time(api);
      }
    }
  }

  if (msg->deviceReport.controller_reports_count > 0) {
    printf("sending sensor report %d\r\n", msg->deviceReport.controller_reports_count);
    send_api_msg(api, msg, api->server_time_available);
  }

  free(msg);
}

static time_t
get_server_time(web_api_t* api)
{
  return (api->last_server_time + ((chTimeNow() - api->last_server_systime) / CH_FREQUENCY));
}

static void
request_activation_token(web_api_t* api)
{
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_ACTIVATION_TOKEN_REQUEST;
  msg->has_activationTokenRequest = true;
  strcpy(msg->activationTokenRequest.device_id, device_id);

  send_api_msg(api, msg, false);

  free(msg);
}

static void
request_auth(web_api_t* api)
{
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_AUTH_REQUEST;
  msg->has_authRequest = true;
  strncpy(msg->authRequest.device_id, device_id, sizeof(msg->authRequest.device_id));
  strncpy(msg->authRequest.auth_token, app_cfg_get_auth_token(), sizeof(msg->authRequest.auth_token));

  msg->authRequest.has_firmware_version = true;
  strncpy(msg->authRequest.firmware_version, VERSION_STR, sizeof(msg->authRequest.firmware_version));

  send_api_msg(api, msg, false);

  free(msg);
}

static void
dispatch_net_status(web_api_t* api, net_status_t* ns)
{
  if (ns->net_state == NS_CONNECTED &&
      ns->dhcp_resolved) {
    if (api->status.state == AS_AWAITING_NET_CONNECTION)
      set_state(api, AS_CONNECTING);
  }
  else {
    set_state(api, AS_AWAITING_NET_CONNECTION);
  }
}

static void
dispatch_sensor_sample(web_api_t* api, sensor_msg_t* sample)
{
  if (sample->sensor >= NUM_SENSORS)
    return;

  api_controller_status_t* s = &api->controller_status[sample->sensor];
  s->new_sample = true;
  s->last_sample = sample->sample;
}

static void
dispatch_device_settings_from_device(
    web_api_t* api,
    output_ctrl_t* control_mode)
{
  printf("device settings updated\r\n");

  if (control_mode != NULL)
    api->new_device_settings = true;
}

static void
dispatch_controller_settings_from_device(
    web_api_t* api,
    controller_settings_t* ssm)
{
  printf("controller settings updated\r\n");

  if (ssm != NULL)
    api->controller_status[ssm->controller].new_settings = true;
}

static void
send_device_settings(
    web_api_t* api)
{
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_DEVICE_SETTINGS;
  msg->has_deviceSettings = true;

  msg->deviceSettings.name[0] = 0;

  output_ctrl_t control_mode = app_cfg_get_control_mode();
  switch (control_mode) {
    case PID:
      msg->deviceSettings.control_mode = DeviceSettings_ControlMode_PID;
      break;

    case ON_OFF:
      msg->deviceSettings.control_mode = DeviceSettings_ControlMode_ON_OFF;
      break;

    default:
      printf("Invalid output control mode: %d\r\n", control_mode);
      break;
  }

  printf("Sending device settings\r\n");
  send_api_msg(api, msg, true);

  free(msg);
}

static void
send_controller_settings(
    web_api_t* api)
{
  int i;
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_CONTROLLER_SETTINGS;
  msg->has_controllerSettings = true;

  for (i = 0; i < NUM_CONTROLLERS; ++i) {
    if (api->controller_status[i].new_settings) {
      api->controller_status[i].new_settings = false;
      const controller_settings_t* ssl = app_cfg_get_controller_settings(i);
      ControllerSettings* ss = &msg->controllerSettings;

      ss->has_session_action = true;
      ss->session_action = ssl->session_action;

      ss->sensor_index = i;
      switch (ssl->setpoint_type) {
        case SP_STATIC:
          ss->setpoint_type = ControllerSettings_SetpointType_STATIC;
          ss->has_static_setpoint = true;
          ss->static_setpoint = ssl->static_setpoint.value;
          break;

        case SP_TEMP_PROFILE:
          ss->setpoint_type = ControllerSettings_SetpointType_TEMP_PROFILE;
          ss->has_temp_profile_id = true;
          ss->temp_profile_id = ssl->temp_profile.id;
          ss->has_temp_profile_completion_action = true;
          ss->temp_profile_completion_action = ControllerSettings_CompletionAction_HOLD_LAST;
          ss->has_temp_profile_start_point = true;
          ss->temp_profile_start_point = 0;
          break;

        default:
          printf("Invalid setpoint type: %d\r\n", ssl->setpoint_type);
          break;
      }

      int j;
      for (j = 0; j < NUM_OUTPUTS; ++j) {
        const output_settings_t* osl = &ssl->output_settings[j];
        if (osl->enabled) {
          OutputSettings* os = &msg->controllerSettings.output_settings[msg->controllerSettings.output_settings_count];
          msg->controllerSettings.output_settings_count++;

          os->index = j;
          os->function = osl->function;
          os->cycle_delay = osl->cycle_delay.value;
        }
      }

      printf("Sending controller settings\r\n");
      send_api_msg(api, msg, true);
    }
  }

  free(msg);
}

static void
check_for_update(web_api_t* api)
{
  printf("sending update check\r\n");
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_FIRMWARE_UPDATE_CHECK_REQUEST;
  msg->has_firmwareUpdateCheckRequest = true;
  sprintf(msg->firmwareUpdateCheckRequest.current_version, "%d.%d.%d", MAJOR_VERSION, MINOR_VERSION, PATCH_VERSION);

  send_api_msg(api, msg, false);

  free(msg);
}

static void
dispatch_firmware_rqst(web_api_t* api, firmware_update_t* firmware_data)
{
  ApiMessage* msg = calloc(1, sizeof(ApiMessage));
  msg->type = ApiMessage_Type_FIRMWARE_DOWNLOAD_REQUEST;
  msg->has_firmwareDownloadRequest = true;
  msg->firmwareDownloadRequest.offset = firmware_data->offset;
  msg->firmwareDownloadRequest.size = firmware_data->size;
  strncpy(msg->firmwareDownloadRequest.requested_version,
      firmware_data->version,
      sizeof(msg->firmwareDownloadRequest.requested_version));

  send_api_msg(api, msg, false);

  free(msg);
}

static void
send_api_msg(web_api_t* api, ApiMessage* msg, bool can_backlog)
{
  uint8_t* buffer = malloc(ApiMessage_size);

  pb_ostream_t stream = pb_ostream_from_buffer(buffer, ApiMessage_size);
  bool encoded_ok = pb_encode(&stream, ApiMessage_fields, msg);

  if (encoded_ok) {
    uint32_t buf_len = htonl(stream.bytes_written);
    if (send_or_store(api, &buf_len, sizeof(buf_len), can_backlog)) {
      if (!send_or_store(api, buffer, stream.bytes_written, can_backlog)) {
        printf("buffer send failed!\r\n");
      }
    }
    else {
      printf("message length send failed!\r\n");
    }
  }

  free(buffer);
}

static bool
send_or_store(web_api_t* api, void* buf, uint32_t buf_len, bool can_backlog)
{
  if (api->status.state > AS_CONNECTING) {
    return socket_send(api, buf, buf_len);
  }
  else {
    if (!can_backlog) {
      printf("Unable to save message to backlog!\r\n");
      return false;
    }

    printf("Not connected. Saving to backlog %d\r\n", (int)api->backlog_pos);
    bool ret = sxfs_write(SP_WEB_API_BACKLOG, api->backlog_pos, buf, buf_len);
    if (ret)
      api->backlog_pos += buf_len;
    return ret;
  }
}

static bool
socket_send(web_api_t* api, void* buf, uint32_t buf_len)
{
  int bytes_left = buf_len;
  while (bytes_left > 0) {
    int ret = send(api->socket, buf, bytes_left, 0);
    if (ret < 0) {
      printf("send failed %d %d\r\n", ret, errno);
      if ((errno != EAGAIN && errno != EWOULDBLOCK) ||
          (++api->send_errors > MAX_SEND_ERRS)) {
        printf("socket disconnected %d\r\n", (int)api->send_errors);
        closesocket(api->socket);
        api->socket = -1;
        set_state(api, AS_CONNECTING);
      }
      return false;
    }
    bytes_left -= ret;
    buf += ret;
    api->last_send_time = chTimeNow();
  }

  return true;
}

static void
socket_message_rx(web_api_t* api, const uint8_t* data, uint32_t data_len)
{
  ApiMessage* msg = malloc(sizeof(ApiMessage));

  pb_istream_t stream = pb_istream_from_buffer((const uint8_t*)data, data_len);
  bool status = pb_decode(&stream, ApiMessage_fields, msg);

  if (status)
    dispatch_api_msg(api, msg);
  else
    printf("Fucked up message received!\r\n");

  free(msg);
}

static void
dispatch_api_msg(web_api_t* api, ApiMessage* msg)
{
  switch (msg->type) {
  case ApiMessage_Type_ACTIVATION_TOKEN_RESPONSE:
    printf("got activation token: %s\r\n", msg->activationTokenResponse.activation_token);
    strncpy(api->status.activation_token,
        msg->activationTokenResponse.activation_token,
        sizeof(api->status.activation_token));
    set_state(api, AS_AWAITING_ACTIVATION);
    break;

  case ApiMessage_Type_ACTIVATION_NOTIFICATION:
    printf("got auth token: %s\r\n", msg->activationNotification.auth_token);
    app_cfg_set_auth_token(msg->activationNotification.auth_token);
    set_state(api, AS_CONNECTED);
    break;

  case ApiMessage_Type_AUTH_RESPONSE:
    if (msg->authResponse.authenticated) {
      printf("auth succeeded\r\n");
      set_state(api, AS_CONNECTED);
    }
    else {
      printf("auth failed, restarting activation\r\n");
      app_cfg_set_auth_token("");
      request_activation_token(api);
      set_state(api, AS_REQUESTING_ACTIVATION_TOKEN);
    }
    break;

  case ApiMessage_Type_FIRMWARE_UPDATE_CHECK_RESPONSE:
    msg_send(MSG_API_FW_UPDATE_CHECK_RESPONSE, &msg->firmwareUpdateCheckResponse);
    break;

  case ApiMessage_Type_FIRMWARE_DOWNLOAD_RESPONSE:
    msg_send(MSG_API_FW_CHUNK, &msg->firmwareDownloadResponse);
    break;

  case ApiMessage_Type_DEVICE_SETTINGS:
    dispatch_device_settings_from_server(&msg->deviceSettings);
    break;

  case ApiMessage_Type_CONTROLLER_SETTINGS:
    dispatch_controller_settings_from_server(&msg->controllerSettings);
    break;

  case ApiMessage_Type_SERVER_TIME:
    dispatch_server_time(api, &msg->serverTime);
    break;

  default:
    printf("Unsupported API message: %d\r\n", msg->type);
    break;
  }
}

static void
dispatch_device_settings_from_server(DeviceSettings* settings)
{
  printf("got device settings from server\r\n");
  printf("  control mode %d\r\n", settings->control_mode);
  printf("  hysteresis %f\r\n", settings->hysteresis);

  app_cfg_set_control_mode(settings->control_mode);

  quantity_t hysteresis;
  hysteresis.value = settings->hysteresis;
  hysteresis.unit = UNIT_TEMP_DEG_F;
  app_cfg_set_hysteresis(hysteresis);
}

static void
dispatch_controller_settings_from_server(ControllerSettings* settings)
{
  int i;

  printf("got controller settings from server\r\n");

  controller_settings_t* csl = calloc(1, sizeof(controller_settings_t));
  memcpy(csl, app_cfg_get_controller_settings(settings->sensor_index), sizeof(controller_settings_t));

  csl->controller = settings->sensor_index;

  printf("  got %d temp profiles\r\n", settings->temp_profiles_count);
  printf("  got %d output settings\r\n", settings->output_settings_count);
  csl->output_settings[OUTPUT_1].enabled = false;
  csl->output_settings[OUTPUT_2].enabled = false;
  for (i = 0; i < (int)settings->output_settings_count; ++i) {
    OutputSettings* osm = &settings->output_settings[i];
    output_settings_t* os = &csl->output_settings[osm->index];

    os->cycle_delay.value = osm->cycle_delay;
    os->cycle_delay.unit = UNIT_TIME_MIN;
    os->function = osm->function;
    os->enabled = true;

    printf("    output %d\r\n", i);
    printf("      delay %f\r\n", os->cycle_delay.value);
    printf("      function %d\r\n", os->function);
  }

  printf("  got sensor settings\r\n");

  switch (settings->setpoint_type) {
    case ControllerSettings_SetpointType_STATIC:
      if (!settings->has_static_setpoint)
        printf("Sensor settings specified static setpoint, but none provided!\r\n");
      else {
        csl->setpoint_type = SP_STATIC;
        csl->static_setpoint.value = settings->static_setpoint;
        csl->static_setpoint.unit = UNIT_TEMP_DEG_F;
      }
      break;

    case ControllerSettings_SetpointType_TEMP_PROFILE:
      if (!settings->has_temp_profile_id)
        printf("Sensor settings specified temp profile, but no provided!\r\n");
      else {
        csl->setpoint_type = SP_TEMP_PROFILE;

        TempProfile* tpm = &settings->temp_profiles[0];
        csl->temp_profile.id = tpm->id;
        strncpy(csl->temp_profile.name, tpm->name, sizeof(csl->temp_profile.name));
        csl->temp_profile.num_steps = tpm->steps_count;
        csl->temp_profile.start_value.value = tpm->start_value;
        csl->temp_profile.start_value.unit = UNIT_TEMP_DEG_F;
        csl->temp_profile.start_point = settings->temp_profile_start_point;
        csl->temp_profile.completion_action = settings->temp_profile_completion_action;

        printf("    profile '%s' (%d)\r\n", csl->temp_profile.name, (int)csl->temp_profile.id);
        printf("      steps %d\r\n", (int)csl->temp_profile.num_steps);
        printf("      start temp %f\r\n", csl->temp_profile.start_value.value);
        printf("      start point %d\r\n", csl->temp_profile.start_point);
        printf("      completion action %d\r\n", csl->temp_profile.completion_action);

        for (i = 0; i < (int)tpm->steps_count; ++i) {
          temp_profile_step_t* step = &csl->temp_profile.steps[i];
          TempProfileStep* stepm = &tpm->steps[i];

          step->duration = stepm->duration;
          step->value.value = stepm->value;
          step->value.unit = UNIT_TEMP_DEG_F;
          switch(stepm->type) {
            case TempProfileStep_TempProfileStepType_HOLD:
              step->type = STEP_HOLD;
              break;

            case TempProfileStep_TempProfileStepType_RAMP:
              step->type = STEP_RAMP;
              break;

            default:
              printf("Invalid step type: %d\r\n", stepm->type);
              break;
          }
        }
      }
      break;

    default:
      printf("Invalid setpoint type: %d\r\n", settings->setpoint_type);
      break;
  }

  printf("    sensor %d\r\n", csl->controller);
  printf("      setpoint_type %d\r\n", csl->setpoint_type);
  printf("      static %f\r\n", csl->static_setpoint.value);
  printf("      temp profile %d\r\n", (int)csl->temp_profile.id);

  app_cfg_set_controller_settings(csl->controller, SS_SERVER, csl);
  free(csl);
}

static void
dispatch_server_time(web_api_t* api, ServerTime* server_time)
{
  api->last_server_systime = chTimeNow();
  api->last_server_time = server_time->timestamp;
  api->server_time_available = true;
}
