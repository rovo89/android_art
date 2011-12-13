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
 * JDWP TCP socket network code.
 */
#include "jdwp/jdwp_priv.h"
#include "jdwp/jdwp_handler.h"
#include "logging.h"
#include "stringprintf.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define kBasePort           8000
#define kMaxPort            8040

#define kInputBufferSize    8192

#define kMagicHandshake     "JDWP-Handshake"
#define kMagicHandshakeLen  (sizeof(kMagicHandshake)-1)

namespace art {

namespace JDWP {

// fwd
static void netShutdown(JdwpNetState* state);
static void netFree(JdwpNetState* state);

/*
 * JDWP network state.
 *
 * We only talk to one debugger at a time.
 */
struct JdwpNetState : public JdwpNetStateBase {
    short   listenPort;
    int     listenSock;         /* listen for connection from debugger */
    int     wakePipe[2];        /* break out of select */

    struct in_addr remoteAddr;
    unsigned short remotePort;

    bool    awaitingHandshake;  /* waiting for "JDWP-Handshake" */

    /* pending data from the network; would be more efficient as circular buf */
    unsigned char  inputBuffer[kInputBufferSize];
    int     inputCount;

    JdwpNetState() {
        listenPort  = 0;
        listenSock  = -1;
        wakePipe[0] = -1;
        wakePipe[1] = -1;

        awaitingHandshake = false;

        inputCount = 0;
    }
};

static JdwpNetState* netStartup(short port, bool probe);

/*
 * Set up some stuff for transport=dt_socket.
 */
static bool prepareSocket(JdwpState* state, const JdwpOptions* options) {
  unsigned short port;

  if (options->server) {
    if (options->port != 0) {
      /* try only the specified port */
      port = options->port;
      state->netState = netStartup(port, false);
    } else {
      /* scan through a range of ports, binding to the first available */
      for (port = kBasePort; port <= kMaxPort; port++) {
        state->netState = netStartup(port, true);
        if (state->netState != NULL) {
          break;
        }
      }
    }
    if (state->netState == NULL) {
      LOG(ERROR) << "JDWP net startup failed (req port=" << options->port << ")";
      return false;
    }
  } else {
    port = options->port;   // used in a debug msg later
    state->netState = netStartup(-1, false);
  }

  if (options->suspend) {
    LOG(INFO) << "JDWP will wait for debugger on port " << port;
  } else {
    LOG(INFO) << "JDWP will " << (options->server ? "listen" : "connect") << " on port " << port;
  }

  return true;
}

/*
 * Are we still waiting for the handshake string?
 */
static bool awaitingHandshake(JdwpState* state) {
  return state->netState->awaitingHandshake;
}

/*
 * Initialize JDWP stuff.
 *
 * Allocates a new state structure.  If "port" is non-negative, this also
 * tries to bind to a listen port.  If "port" is less than zero, we assume
 * we're preparing for an outbound connection, and return without binding
 * to anything.
 *
 * This may be called several times if we're probing for a port.
 *
 * Returns 0 on success.
 */
static JdwpNetState* netStartup(short port, bool probe) {
  int one = 1;
  JdwpNetState* netState = new JdwpNetState;
  if (port < 0) {
    return netState;
  }

  CHECK_NE(port, 0);

  netState->listenSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (netState->listenSock < 0) {
    PLOG(probe ? ERROR : FATAL) << "Socket create failed";
    goto fail;
  }

  /* allow immediate re-use */
  if (setsockopt(netState->listenSock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    PLOG(probe ? ERROR : FATAL) << "setsockopt(SO_REUSEADDR) failed";
    goto fail;
  }

  union {
    struct sockaddr_in  addrInet;
    struct sockaddr     addrPlain;
  } addr;
  addr.addrInet.sin_family = AF_INET;
  addr.addrInet.sin_port = htons(port);
  inet_aton("127.0.0.1", &addr.addrInet.sin_addr);

  if (bind(netState->listenSock, &addr.addrPlain, sizeof(addr)) != 0) {
    PLOG(probe ? ERROR : FATAL) << "Attempt to bind to port " << port << " failed";
    goto fail;
  }

  netState->listenPort = port;

  if (listen(netState->listenSock, 5) != 0) {
    PLOG(probe ? ERROR : FATAL) << "Listen failed";
    goto fail;
  }

  return netState;

fail:
  netShutdown(netState);
  netFree(netState);
  return NULL;
}

/*
 * Shut down JDWP listener.  Don't free state.
 *
 * Note that "netState" may be partially initialized if "startup" failed.
 *
 * This may be called from a non-JDWP thread as part of shutting the
 * JDWP thread down.
 *
 * (This is currently called several times during startup as we probe
 * for an open port.)
 */
static void netShutdown(JdwpNetState* netState) {
  if (netState == NULL) {
    return;
  }

  int listenSock = netState->listenSock;
  int clientSock = netState->clientSock;

  /* clear these out so it doesn't wake up and try to reuse them */
  netState->listenSock = netState->clientSock = -1;

  /* "shutdown" dislodges blocking read() and accept() calls */
  if (listenSock >= 0) {
    shutdown(listenSock, SHUT_RDWR);
    close(listenSock);
  }
  if (clientSock >= 0) {
    shutdown(clientSock, SHUT_RDWR);
    close(clientSock);
  }

  /* if we might be sitting in select, kick us loose */
  if (netState->wakePipe[1] >= 0) {
    VLOG(jdwp) << "+++ writing to wakePipe";
    (void) write(netState->wakePipe[1], "", 1);
  }
}

static void netShutdownExtern(JdwpState* state) {
  netShutdown(state->netState);
}

/*
 * Free JDWP state.
 *
 * Call this after shutting the network down with netShutdown().
 */
static void netFree(JdwpNetState* netState) {
  if (netState == NULL) {
    return;
  }
  CHECK_EQ(netState->listenSock, -1);
  CHECK_EQ(netState->clientSock, -1);

  if (netState->wakePipe[0] >= 0) {
    close(netState->wakePipe[0]);
    netState->wakePipe[0] = -1;
  }
  if (netState->wakePipe[1] >= 0) {
    close(netState->wakePipe[1]);
    netState->wakePipe[1] = -1;
  }

  delete netState;
}

static void netFreeExtern(JdwpState* state) {
  netFree(state->netState);
}

/*
 * Returns "true" if we're connected to a debugger.
 */
static bool isConnected(JdwpState* state) {
  return (state->netState != NULL && state->netState->clientSock >= 0);
}

/*
 * Returns "true" if the fd is ready, "false" if not.
 */
#if 0
static bool isFdReadable(int sock)
{
    fd_set readfds;
    struct timeval tv;
    int count;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    count = select(sock+1, &readfds, NULL, NULL, &tv);
    if (count <= 0)
        return false;

    if (FD_ISSET(sock, &readfds))   /* make sure it's our fd */
        return true;

  LOG(ERROR) << "WEIRD: odd behavior in select (count=" << count << ")";
  return false;
}
#endif

#if 0
/*
 * Check to see if we have a pending connection from the debugger.
 *
 * Returns true on success (meaning a connection is available).
 */
static bool checkConnection(JdwpState* state) {
    JdwpNetState* netState = state->netState;

    CHECK_GE(netState->listenSock, 0);
    /* not expecting to be called when debugger is actively connected */
    CHECK_LT(netState->clientSock, 0);

    if (!isFdReadable(netState->listenSock))
        return false;
    return true;
}
#endif

/*
 * Disable the TCP Nagle algorithm, which delays transmission of outbound
 * packets until the previous transmissions have been acked.  JDWP does a
 * lot of back-and-forth with small packets, so this may help.
 */
static int setNoDelay(int fd)
{
    int on = 1;
    int cc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    CHECK_EQ(cc, 0);
    return cc;
}

/*
 * Accept a connection.  This will block waiting for somebody to show up.
 * If that's not desirable, use checkConnection() to make sure something
 * is pending.
 */
static bool acceptConnection(JdwpState* state)
{
  JdwpNetState* netState = state->netState;
  union {
    struct sockaddr_in  addrInet;
    struct sockaddr     addrPlain;
  } addr;
  socklen_t addrlen;
  int sock;

  if (netState->listenSock < 0) {
    return false;       /* you're not listening! */
  }

  CHECK_LT(netState->clientSock, 0);      /* must not already be talking */

  addrlen = sizeof(addr);
  do {
    sock = accept(netState->listenSock, &addr.addrPlain, &addrlen);
    if (sock < 0 && errno != EINTR) {
      // When we call shutdown() on the socket, accept() returns with
      // EINVAL.  Don't gripe about it.
      if (errno == EINVAL) {
        if (VLOG_IS_ON(jdwp)) {
          PLOG(ERROR) << "accept failed";
        }
      } else {
        PLOG(ERROR) << "accept failed";
        return false;
      }
    }
  } while (sock < 0);

  netState->remoteAddr = addr.addrInet.sin_addr;
  netState->remotePort = ntohs(addr.addrInet.sin_port);
  VLOG(jdwp) << "+++ accepted connection from " << inet_ntoa(netState->remoteAddr) << ":" << netState->remotePort;

  netState->clientSock = sock;
  netState->awaitingHandshake = true;
  netState->inputCount = 0;

  VLOG(jdwp) << "Setting TCP_NODELAY on accepted socket";
  setNoDelay(netState->clientSock);

  if (pipe(netState->wakePipe) < 0) {
    PLOG(ERROR) << "pipe failed";
    return false;
  }

  return true;
}

/*
 * Create a connection to a waiting debugger.
 */
static bool establishConnection(JdwpState* state) {
  union {
    struct sockaddr_in  addrInet;
    struct sockaddr     addrPlain;
  } addr;
  struct hostent* pEntry;

  CHECK(state != NULL && state->netState != NULL);
  CHECK(!state->options_->server);
  CHECK(!state->options_->host.empty());
  CHECK_NE(state->options_->port, 0);

  /*
   * Start by resolving the host name.
   */
//#undef HAVE_GETHOSTBYNAME_R
//#warning "forcing non-R"
#ifdef HAVE_GETHOSTBYNAME_R
  struct hostent he;
  char auxBuf[128];
  int error;
  int cc = gethostbyname_r(state->options_->host.c_str(), &he, auxBuf, sizeof(auxBuf), &pEntry, &error);
  if (cc != 0) {
    LOG(WARNING) << "gethostbyname_r('" << state->options_->host << "') failed: " << hstrerror(error);
    return false;
  }
#else
  h_errno = 0;
  pEntry = gethostbyname(state->options_->host.c_str());
  if (pEntry == NULL) {
    PLOG(WARNING) << "gethostbyname('" << state->options_->host << "') failed";
    return false;
  }
#endif

  /* copy it out ASAP to minimize risk of multithreaded annoyances */
  memcpy(&addr.addrInet.sin_addr, pEntry->h_addr, pEntry->h_length);
  addr.addrInet.sin_family = pEntry->h_addrtype;

  addr.addrInet.sin_port = htons(state->options_->port);

  LOG(INFO) << "Connecting out to " << inet_ntoa(addr.addrInet.sin_addr) << ":" << ntohs(addr.addrInet.sin_port);

  /*
   * Create a socket.
   */
  JdwpNetState* netState;
  netState = state->netState;
  netState->clientSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (netState->clientSock < 0) {
    PLOG(ERROR) << "Unable to create socket";
    return false;
  }

  /*
   * Try to connect.
   */
  if (connect(netState->clientSock, &addr.addrPlain, sizeof(addr)) != 0) {
    PLOG(ERROR) << "Unable to connect to " << inet_ntoa(addr.addrInet.sin_addr) << ":" << ntohs(addr.addrInet.sin_port);
    close(netState->clientSock);
    netState->clientSock = -1;
    return false;
  }

  LOG(INFO) << "Connection established to " << state->options_->host << " (" << inet_ntoa(addr.addrInet.sin_addr) << ":" << ntohs(addr.addrInet.sin_port) << ")";
  netState->awaitingHandshake = true;
  netState->inputCount = 0;

  setNoDelay(netState->clientSock);

  if (pipe(netState->wakePipe) < 0) {
    PLOG(ERROR) << "pipe failed";
    return false;
  }

  return true;
}

/*
 * Close the connection to the debugger.
 *
 * Reset the state so we're ready to receive a new connection.
 */
static void closeConnection(JdwpState* state) {
  JdwpNetState* netState;

  CHECK(state != NULL && state->netState != NULL);

  netState = state->netState;
  if (netState->clientSock < 0) {
    return;
  }

  VLOG(jdwp) << "+++ closed connection to " << inet_ntoa(netState->remoteAddr) << ":" << netState->remotePort;

  close(netState->clientSock);
  netState->clientSock = -1;
}

/*
 * Figure out if we have a full packet in the buffer.
 */
static bool haveFullPacket(JdwpNetState* netState) {
  if (netState->awaitingHandshake) {
    return (netState->inputCount >= (int) kMagicHandshakeLen);
  }
  if (netState->inputCount < 4) {
    return false;
  }
  long length = Get4BE(netState->inputBuffer);
  return (netState->inputCount >= length);
}

/*
 * Consume bytes from the buffer.
 *
 * This would be more efficient with a circular buffer.  However, we're
 * usually only going to find one packet, which is trivial to handle.
 */
static void consumeBytes(JdwpNetState* netState, int count) {
  CHECK_GT(count, 0);
  CHECK_LE(count, netState->inputCount);

  if (count == netState->inputCount) {
    netState->inputCount = 0;
    return;
  }

  memmove(netState->inputBuffer, netState->inputBuffer + count, netState->inputCount - count);
  netState->inputCount -= count;
}

/*
 * Dump the contents of a packet to stdout.
 */
#if 0
static void dumpPacket(const unsigned char* packetBuf) {
  const unsigned char* buf = packetBuf;
  uint32_t length, id;
  uint8_t flags, cmdSet, cmd;
  uint16_t error;
  bool reply;
  int dataLen;

  cmd = cmdSet = 0xcc;

  length = Read4BE(&buf);
  id = Read4BE(&buf);
  flags = Read1(&buf);
  if ((flags & kJDWPFlagReply) != 0) {
    reply = true;
    error = Read2BE(&buf);
  } else {
    reply = false;
    cmdSet = Read1(&buf);
    cmd = Read1(&buf);
  }

  dataLen = length - (buf - packetBuf);

  VLOG(jdwp) << StringPrintf("--- %s: dataLen=%u id=0x%08x flags=0x%02x cmd=%d/%d",
      reply ? "reply" : "req", dataLen, id, flags, cmdSet, cmd);
  if (dataLen > 0) {
    HexDump(buf, dataLen);
  }
}
#endif

/*
 * Handle a packet.  Returns "false" if we encounter a connection-fatal error.
 */
static bool handlePacket(JdwpState* state) {
  JdwpNetState* netState = state->netState;
  const unsigned char* buf = netState->inputBuffer;
  JdwpReqHeader hdr;
  uint32_t length, id;
  uint8_t flags, cmdSet, cmd;
  uint16_t error;
  bool reply;
  int dataLen;

  cmd = cmdSet = 0;       // shut up gcc

  /*dumpPacket(netState->inputBuffer);*/

  length = Read4BE(&buf);
  id = Read4BE(&buf);
  flags = Read1(&buf);
  if ((flags & kJDWPFlagReply) != 0) {
    reply = true;
    error = Read2BE(&buf);
  } else {
    reply = false;
    cmdSet = Read1(&buf);
    cmd = Read1(&buf);
  }

  CHECK_LE((int) length, netState->inputCount);
  dataLen = length - (buf - netState->inputBuffer);

  if (!reply) {
    ExpandBuf* pReply = expandBufAlloc();

    hdr.length = length;
    hdr.id = id;
    hdr.cmdSet = cmdSet;
    hdr.cmd = cmd;
    state->ProcessRequest(&hdr, buf, dataLen, pReply);
    if (expandBufGetLength(pReply) > 0) {
      ssize_t cc = netState->writePacket(pReply);

      if (cc != (ssize_t) expandBufGetLength(pReply)) {
        PLOG(ERROR) << "Failed sending reply to debugger";
        expandBufFree(pReply);
        return false;
      }
    } else {
      LOG(WARNING) << "No reply created for set=" << cmdSet << " cmd=" << cmd;
    }
    expandBufFree(pReply);
  } else {
    LOG(ERROR) << "reply?!";
    DCHECK(false);
  }

  VLOG(jdwp) << "----------";

  consumeBytes(netState, length);
  return true;
}

/*
 * Process incoming data.  If no data is available, this will block until
 * some arrives.
 *
 * If we get a full packet, handle it.
 *
 * To take some of the mystery out of life, we want to reject incoming
 * connections if we already have a debugger attached.  If we don't, the
 * debugger will just mysteriously hang until it times out.  We could just
 * close the listen socket, but there's a good chance we won't be able to
 * bind to the same port again, which would confuse utilities.
 *
 * Returns "false" on error (indicating that the connection has been severed),
 * "true" if things are still okay.
 */
static bool processIncoming(JdwpState* state) {
  JdwpNetState* netState = state->netState;
  int readCount;

  CHECK_GE(netState->clientSock, 0);

  if (!haveFullPacket(netState)) {
    /* read some more, looping until we have data */
    errno = 0;
    while (1) {
      int selCount;
      fd_set readfds;
      int maxfd;
      int fd;

      maxfd = netState->listenSock;
      if (netState->clientSock > maxfd) {
        maxfd = netState->clientSock;
      }
      if (netState->wakePipe[0] > maxfd) {
        maxfd = netState->wakePipe[0];
      }

      if (maxfd < 0) {
        VLOG(jdwp) << "+++ all fds are closed";
        return false;
      }

      FD_ZERO(&readfds);

      /* configure fds; note these may get zapped by another thread */
      fd = netState->listenSock;
      if (fd >= 0) {
        FD_SET(fd, &readfds);
      }
      fd = netState->clientSock;
      if (fd >= 0) {
        FD_SET(fd, &readfds);
      }
      fd = netState->wakePipe[0];
      if (fd >= 0) {
        FD_SET(fd, &readfds);
      } else {
        LOG(INFO) << "NOTE: entering select w/o wakepipe";
      }

      /*
       * Select blocks until it sees activity on the file descriptors.
       * Closing the local file descriptor does not count as activity,
       * so we can't rely on that to wake us up (it works for read()
       * and accept(), but not select()).
       *
       * We can do one of three things: (1) send a signal and catch
       * EINTR, (2) open an additional fd ("wakePipe") and write to
       * it when it's time to exit, or (3) time out periodically and
       * re-issue the select.  We're currently using #2, as it's more
       * reliable than #1 and generally better than #3.  Wastes two fds.
       */
      selCount = select(maxfd+1, &readfds, NULL, NULL, NULL);
      if (selCount < 0) {
        if (errno == EINTR) {
          continue;
        }
        PLOG(ERROR) << "select failed";
        goto fail;
      }

      if (netState->wakePipe[0] >= 0 && FD_ISSET(netState->wakePipe[0], &readfds)) {
        if (netState->listenSock >= 0) {
          LOG(ERROR) << "Exit wake set, but not exiting?";
        } else {
          LOG(DEBUG) << "Got wake-up signal, bailing out of select";
        }
        goto fail;
      }
      if (netState->listenSock >= 0 && FD_ISSET(netState->listenSock, &readfds)) {
        LOG(INFO) << "Ignoring second debugger -- accepting and dropping";
        union {
          struct sockaddr_in   addrInet;
          struct sockaddr      addrPlain;
        } addr;
        socklen_t addrlen;
        int tmpSock;
        tmpSock = accept(netState->listenSock, &addr.addrPlain, &addrlen);
        if (tmpSock < 0) {
          LOG(INFO) << "Weird -- accept failed";
        } else {
          close(tmpSock);
        }
      }
      if (netState->clientSock >= 0 && FD_ISSET(netState->clientSock, &readfds)) {
        readCount = read(netState->clientSock, netState->inputBuffer + netState->inputCount, sizeof(netState->inputBuffer) - netState->inputCount);
        if (readCount < 0) {
          /* read failed */
          if (errno != EINTR) {
            goto fail;
          }
          LOG(DEBUG) << "+++ EINTR hit";
          return true;
        } else if (readCount == 0) {
          /* EOF hit -- far end went away */
          LOG(DEBUG) << "+++ peer disconnected";
          goto fail;
        } else {
          break;
        }
      }
    }

    netState->inputCount += readCount;
    if (!haveFullPacket(netState)) {
      return true;        /* still not there yet */
    }
  }

  /*
   * Special-case the initial handshake.  For some bizarre reason we're
   * expected to emulate bad tty settings by echoing the request back
   * exactly as it was sent.  Note the handshake is always initiated by
   * the debugger, no matter who connects to whom.
   *
   * Other than this one case, the protocol [claims to be] stateless.
   */
  if (netState->awaitingHandshake) {
    int cc;

    if (memcmp(netState->inputBuffer, kMagicHandshake, kMagicHandshakeLen) != 0) {
      LOG(ERROR) << StringPrintf("ERROR: bad handshake '%.14s'", netState->inputBuffer);
      goto fail;
    }

    errno = 0;
    cc = write(netState->clientSock, netState->inputBuffer, kMagicHandshakeLen);
    if (cc != kMagicHandshakeLen) {
      PLOG(ERROR) << "Failed writing handshake bytes (" << cc << " of " << kMagicHandshakeLen << ")";
      goto fail;
    }

    consumeBytes(netState, kMagicHandshakeLen);
    netState->awaitingHandshake = false;
    VLOG(jdwp) << "+++ handshake complete";
    return true;
  }

  /*
   * Handle this packet.
   */
  return handlePacket(state);

fail:
  closeConnection(state);
  return false;
}

/*
 * Send a request.
 *
 * The entire packet must be sent with a single write() call to avoid
 * threading issues.
 *
 * Returns "true" if it was sent successfully.
 */
static bool sendRequest(JdwpState* state, ExpandBuf* pReq) {
  JdwpNetState* netState = state->netState;

  /*dumpPacket(expandBufGetBuffer(pReq));*/
  if (netState->clientSock < 0) {
    /* can happen with some DDMS events */
    VLOG(jdwp) << "NOT sending request -- no debugger is attached";
    return false;
  }

  errno = 0;
  ssize_t cc = netState->writePacket(pReq);

  if (cc != (ssize_t) expandBufGetLength(pReq)) {
    PLOG(ERROR) << "Failed sending req to debugger (" << cc << " of " << expandBufGetLength(pReq) << ")";
    return false;
  }

  return true;
}

/*
 * Send a request that was split into multiple buffers.
 *
 * The entire packet must be sent with a single writev() call to avoid
 * threading issues.
 *
 * Returns "true" if it was sent successfully.
 */
static bool sendBufferedRequest(JdwpState* state, const iovec* iov, int iov_count) {
  JdwpNetState* netState = state->netState;

  if (netState->clientSock < 0) {
    /* can happen with some DDMS events */
    VLOG(jdwp) << "NOT sending request -- no debugger is attached";
    return false;
  }

  size_t expected = 0;
  for (int i = 0; i < iov_count; i++) {
    expected += iov[i].iov_len;
  }

  ssize_t actual = netState->writeBufferedPacket(iov, iov_count);

  if ((size_t)actual != expected) {
    PLOG(ERROR) << "Failed sending b-req to debugger (" << actual << " of " << expected << ")";
    return false;
  }

  return true;
}

/*
 * Our functions.
 *
 * We can't generally share the implementations with other transports,
 * even if they're also socket-based, because our JdwpNetState will be
 * different from theirs.
 */
static const JdwpTransport socketTransport = {
  prepareSocket,
  acceptConnection,
  establishConnection,
  closeConnection,
  netShutdownExtern,
  netFreeExtern,
  isConnected,
  awaitingHandshake,
  processIncoming,
  sendRequest,
  sendBufferedRequest,
};

/*
 * Return our set.
 */
const JdwpTransport* SocketTransport() {
  return &socketTransport;
}

}  // namespace JDWP

}  // namespace art
