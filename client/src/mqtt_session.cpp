#include <oa3/client/mqtt_session.h>

namespace oa3 {

const char* to_string(MqttResult r) {
    switch (r) {
        case MqttResult::kOk:
            return "OK";
        case MqttResult::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case MqttResult::kNetworkDown:
            return "NETWORK_DOWN";
        case MqttResult::kDnsFailed:
            return "DNS_FAILED";
        case MqttResult::kConnectFailed:
            return "CONNECT_FAILED";
        case MqttResult::kTlsFailed:
            return "TLS_FAILED";
        case MqttResult::kTlsVerifyFailed:
            return "TLS_VERIFY_FAILED";
        case MqttResult::kRefused:
            return "REFUSED";
        case MqttResult::kNotAuthorized:
            return "NOT_AUTHORIZED";
        case MqttResult::kSubscribeFailed:
            return "SUBSCRIBE_FAILED";
        case MqttResult::kTimeout:
            return "TIMEOUT";
        case MqttResult::kNotConnected:
            return "NOT_CONNECTED";
        case MqttResult::kConnectionLost:
            return "CONNECTION_LOST";
        case MqttResult::kIoError:
            return "IO_ERROR";
    }
    return "UNKNOWN";
}

}  // namespace oa3
