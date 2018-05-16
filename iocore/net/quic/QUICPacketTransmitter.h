/** @file
 *
 *  A brief file description
 *
 *  @section license License
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#pragma once

#include "QUICPacket.h"
#include "I_EventSystem.h"

class QUICPacketTransmitter
{
public:
  /*
   * Enqueue a packet for transmission
   *
   * If packet parameter is not passed, it just sends an event without queuing a new packet.
   * This sends QUIC_PACKET_WRITE_READY event.
   * This return number of packets currently in queue
   */
  virtual uint32_t transmit_packet(QUICPacketUPtr packet = QUICPacketUPtr(nullptr, &QUICPacketDeleter::delete_packet)) = 0;

  /*
   * Enqueue a packet for retransmission
   * All frames except ACK and PADDING frames in the original packet will be retransmitted on a new packet.
   * This sends QUIC_PACKET_WRITE_READY event.
   */
  virtual void retransmit_packet(const QUICPacket &packet) = 0;

  /*
   * Returns a mutex for transmitter interfaces.
   * You have to acquire a lock with this mutex before calling any methods provieded by QUICPacketTransmitter
   */
  virtual Ptr<ProxyMutex> get_packet_transmitter_mutex() = 0;
};