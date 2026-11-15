#include "MyServerTask.h"
namespace MyNameSpace {
bool MyServerTask::cmdParse(const char* payload,int length) {
    return length>0 && sendDataWithBuffer(payload,static_cast<std::uint32_t>(length))>0;
}
}
