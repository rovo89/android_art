/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * JDWP internal interfaces.
 */
#ifndef ART_JDWP_JDWPPRIV_H_
#define ART_JDWP_JDWPPRIV_H_

#include "debugger.h"
#include "jdwp/jdwp.h"
#include "jdwp/jdwp_event.h"

#include <pthread.h>
#include <sys/uio.h>

/*
 * JDWP constants.
 */
#define kJDWPHeaderLen  11
#define kJDWPFlagReply  0x80

/* DDM support */
#define kJDWPDdmCmdSet  199     /* 0xc7, or 'G'+128 */
#define kJDWPDdmCmd     1

namespace art {

namespace JDWP {

/*
 * Transport-specific network status.
 */
struct JdwpNetState;
struct JdwpState;

/*
 * Transport functions.
 */
struct JdwpTransport {
  bool (*startup)(JdwpState* state, const JdwpOptions* options);
  bool (*accept)(JdwpState* state);
  bool (*establish)(JdwpState* state);
  void (*close)(JdwpState* state);
  void (*shutdown)(JdwpState* state);
  void (*free)(JdwpState* state);
  bool (*isConnected)(JdwpState* state);
  bool (*awaitingHandshake)(JdwpState* state);
  bool (*processIncoming)(JdwpState* state);
  bool (*sendRequest)(JdwpState* state, ExpandBuf* pReq);
  bool (*sendBufferedRequest)(JdwpState* state, const iovec* iov, int iov_count);
};

const JdwpTransport* SocketTransport();
const JdwpTransport* AndroidAdbTransport();

/*
 * Base class for JdwpNetState
 */
class JdwpNetStateBase {
public:
  int clientSock;     /* active connection to debugger */

  JdwpNetStateBase();
  ssize_t writePacket(ExpandBuf* pReply);
  ssize_t writeBufferedPacket(const iovec* iov, int iov_count);

private:
  Mutex socket_lock_;
};

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_JDWPPRIV_H_
