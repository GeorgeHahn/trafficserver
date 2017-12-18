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

#include "QUICLossDetector.h"
#include "QUICEvents.h"
#include "ts/ink_assert.h"

#define QUICLDDebug(fmt, ...) \
  Debug("quic_loss_detector", "[%" PRIx64 "] " fmt, static_cast<uint64_t>(this->_connection_id), ##__VA_ARGS__)

QUICLossDetector::QUICLossDetector(QUICPacketTransmitter *transmitter, QUICCongestionController *cc)
  : _transmitter(transmitter), _cc(cc)
{
  this->mutex = new_ProxyMutex();

  if (this->_time_loss_detection) {
    this->_reordering_threshold     = UINT32_MAX;
    this->_time_reordering_fraction = this->_TIME_REORDERING_FRACTION;
  } else {
    this->_reordering_threshold     = this->_REORDERING_THRESHOLD;
    this->_time_reordering_fraction = INFINITY;
  }

  this->_handshake_outstanding       = 0;
  this->_retransmittable_outstanding = 0;

  SET_HANDLER(&QUICLossDetector::event_handler);
}

int
QUICLossDetector::event_handler(int event, Event *edata)
{
  switch (event) {
  case EVENT_INTERVAL: {
    if (this->_loss_detection_alarm_at <= Thread::get_hrtime()) {
      this->_on_loss_detection_alarm();
    }
    break;
  }
  case QUIC_EVENT_LD_SHUTDOWN: {
    SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
    QUICLDDebug("Shutdown");

    if (this->_loss_detection_alarm) {
      this->_loss_detection_alarm->cancel();
    }
    break;
  }
  default:
    break;
  }
  return EVENT_CONT;
}

std::vector<QUICFrameType>
QUICLossDetector::interests()
{
  return {QUICFrameType::ACK};
}

QUICErrorUPtr
QUICLossDetector::handle_frame(std::shared_ptr<const QUICFrame> frame)
{
  QUICErrorUPtr error = QUICErrorUPtr(new QUICNoError());

  switch (frame->type()) {
  case QUICFrameType::ACK:
    this->_on_ack_received(std::dynamic_pointer_cast<const QUICAckFrame>(frame));
    break;
  default:
    QUICLDDebug("Unexpected frame type: %02x", static_cast<unsigned int>(frame->type()));
    ink_assert(false);
    break;
  }

  return error;
}

QUICPacketNumber
QUICLossDetector::largest_acked_packet_number()
{
  return this->_largest_acked_packet;
}

void
QUICLossDetector::_detect_lost_packets(QUICPacketNumber largest_acked_packet_number)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  this->_loss_time = 0;
  std::set<QUICPacketNumber> lost_packets;
  uint32_t delay_until_lost = UINT32_MAX;

  if (this->_time_reordering_fraction != INFINITY) {
    delay_until_lost = (1 + this->_time_reordering_fraction) * std::max(this->_latest_rtt, this->_smoothed_rtt);
  } else if (largest_acked_packet_number == this->_largest_sent_packet) {
    // Early retransmit alarm.
    delay_until_lost = 9 / 8 * std::max(this->_latest_rtt, this->_smoothed_rtt);
  }
  for (auto &unacked : this->_sent_packets) {
    if (unacked.first >= largest_acked_packet_number) {
      break;
    }
    ink_hrtime time_since_sent = Thread::get_hrtime() - unacked.second->time;
    uint64_t packet_delta      = largest_acked_packet_number - unacked.second->packet_number;
    if (time_since_sent > delay_until_lost) {
      lost_packets.insert(unacked.first);
    } else if (packet_delta > this->_reordering_threshold) {
      lost_packets.insert(unacked.first);
    } else if (this->_loss_time == 0 && delay_until_lost != INFINITY) {
      this->_loss_time = Thread::get_hrtime() + delay_until_lost - time_since_sent;
    }
  }

  // Inform the congestion controller of lost packets and
  // lets it decide whether to retransmit immediately.
  if (!lost_packets.empty()) {
    this->_cc->on_packets_lost(lost_packets);
    for (auto packet_number : lost_packets) {
      this->_decrement_packet_count(packet_number);
      this->_sent_packets.erase(packet_number);
    }
  }
}

void
QUICLossDetector::on_packet_sent(QUICPacketUPtr packet)
{
  if (this->_connection_id == 0) {
    this->_connection_id = packet->connection_id();
  }

  bool is_handshake   = false;
  QUICPacketType type = packet->type();

  // XXX: Should QUICPacketType::SERVER_STATELESS_RETRY be included?
  if (type == QUICPacketType::INITIAL || type == QUICPacketType::HANDSHAKE) {
    is_handshake = true;
  }

  QUICPacketNumber packet_number = packet->packet_number();
  bool is_retransmittable        = packet->is_retransmittable();
  size_t sent_bytes              = packet->size();
  return this->_on_packet_sent(packet_number, is_retransmittable, is_handshake, sent_bytes, std::move(packet));
}

void
QUICLossDetector::_on_packet_sent(QUICPacketNumber packet_number, bool is_retransmittable, bool is_handshake, size_t sent_bytes,
                                  QUICPacketUPtr packet)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  this->_largest_sent_packet      = packet_number;
  this->_time_of_last_sent_packet = Thread::get_hrtime();
  // FIXME Should we really keep actual packet object?

  std::unique_ptr<PacketInfo> packet_info(new PacketInfo(
    {packet_number, this->_time_of_last_sent_packet, is_retransmittable, is_handshake, sent_bytes, std::move(packet)}));
  this->_sent_packets.insert(std::pair<QUICPacketNumber, std::unique_ptr<PacketInfo>>(packet_number, std::move(packet_info)));
  if (is_handshake) {
    ++this->_handshake_outstanding;
  }
  if (is_retransmittable) {
    ++this->_retransmittable_outstanding;
    this->_set_loss_detection_alarm();
  }
}

void
QUICLossDetector::_on_ack_received(const std::shared_ptr<const QUICAckFrame> &ack_frame)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  this->_largest_acked_packet = ack_frame->largest_acknowledged();
  // If the largest acked is newly acked, update the RTT.
  auto pi = this->_sent_packets.find(ack_frame->largest_acknowledged());
  if (pi != this->_sent_packets.end()) {
    this->_latest_rtt = Thread::get_hrtime() - pi->second->time;
    // _latest_rtt is nanosecond but ack_frame->ack_delay is millisecond
    if (this->_latest_rtt > static_cast<ink_hrtime>(HRTIME_MSECONDS(ack_frame->ack_delay()))) {
      this->_latest_rtt -= HRTIME_MSECONDS(ack_frame->ack_delay());
    }
    this->_update_rtt(this->_latest_rtt);
  }

  QUICLDDebug("Unacked packets %lu (retransmittable %u, includes %u handshake packets)", this->_sent_packets.size(),
              this->_retransmittable_outstanding.load(), this->_handshake_outstanding.load());

  // Find all newly acked packets.
  for (auto acked_packet_number : this->_determine_newly_acked_packets(*ack_frame)) {
    this->_on_packet_acked(acked_packet_number);
  }

  QUICLDDebug("Unacked packets %lu (retransmittable %u, includes %u handshake packets)", this->_sent_packets.size(),
              this->_retransmittable_outstanding.load(), this->_handshake_outstanding.load());

  this->_detect_lost_packets(ack_frame->largest_acknowledged());

  QUICLDDebug("Unacked packets %lu (retransmittable %u, includes %u handshake packets)", this->_sent_packets.size(),
              this->_retransmittable_outstanding.load(), this->_handshake_outstanding.load());

  this->_set_loss_detection_alarm();
}

void
QUICLossDetector::_on_packet_acked(QUICPacketNumber acked_packet_number)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  QUICLDDebug("Packet number %" PRIu64 " has been acked", acked_packet_number);
  // If a packet sent prior to RTO was acked, then the RTO
  // was spurious.  Otherwise, inform congestion control.
  if (this->_rto_count > 0 && acked_packet_number > this->_largest_sent_before_rto) {
    // TODO cc->on_retransmission_timeout_verified();
  }
  this->_handshake_count = 0;
  this->_tlp_count       = 0;
  this->_rto_count       = 0;
  this->_decrement_packet_count(acked_packet_number);
  this->_sent_packets.erase(acked_packet_number);
}

void
QUICLossDetector::_decrement_packet_count(QUICPacketNumber packet_number)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  auto ite = this->_sent_packets.find(packet_number);
  if (ite != this->_sent_packets.end()) {
    if (ite->second->handshake) {
      --this->_handshake_outstanding;
    }
    if (ite->second->retransmittable) {
      --this->_retransmittable_outstanding;
    }
  }
}

void
QUICLossDetector::_on_loss_detection_alarm()
{
  if (this->_handshake_outstanding) {
    // Handshake retransmission alarm.
    this->_retransmit_handshake_packets();
    this->_handshake_count++;
  } else if (this->_loss_time != 0) {
    // Early retransmit or Time Loss Detection
    this->_detect_lost_packets(this->_largest_acked_packet);
  } else if (this->_tlp_count < this->_MAX_TLPS) {
    // Tail Loss Probe.
    QUICLDDebug("TLP");
    this->_send_one_packet();
    this->_tlp_count++;
  } else {
    // RTO.
    if (this->_rto_count == 0) {
      this->_largest_sent_before_rto = this->_largest_sent_packet;
    }
    QUICLDDebug("RTO");
    this->_send_two_packets();
    this->_rto_count++;
  }
  QUICLDDebug("Unacked packets %lu (retransmittable %u, includes %u handshake packets)", this->_sent_packets.size(),
              this->_retransmittable_outstanding.load(), this->_handshake_outstanding.load());
  this->_set_loss_detection_alarm();
}

void
QUICLossDetector::_update_rtt(ink_hrtime latest_rtt)
{
  // Based on {{RFC6298}}.
  if (this->_smoothed_rtt == 0) {
    this->_smoothed_rtt = latest_rtt;
    this->_rttvar       = latest_rtt / 2;
  } else {
    this->_rttvar       = 3 / 4 * this->_rttvar + 1 / 4 * ABS(this->_smoothed_rtt - latest_rtt);
    this->_smoothed_rtt = 7 / 8 * this->_smoothed_rtt + 1 / 8 * latest_rtt;
  }
}

void
QUICLossDetector::_set_loss_detection_alarm()
{
  ink_hrtime alarm_duration;
  if (!this->_retransmittable_outstanding && this->_loss_detection_alarm) {
    this->_loss_detection_alarm_at = 0;
    this->_loss_detection_alarm->cancel();
    this->_loss_detection_alarm = nullptr;
    QUICLDDebug("Loss detection alarm has been unset");
    return;
  }
  if (this->_handshake_outstanding) {
    // Handshake retransmission alarm.
    if (this->_smoothed_rtt == 0) {
      alarm_duration = 2 * this->_DEFAULT_INITIAL_RTT;
    } else {
      alarm_duration = 2 * this->_smoothed_rtt;
    }
    alarm_duration = std::max(alarm_duration, this->_MIN_TLP_TIMEOUT);
    alarm_duration = alarm_duration * (1 << this->_handshake_count);
    QUICLDDebug("Handshake retransmission alarm will be set");
  } else if (this->_loss_time != 0) {
    // Early retransmit timer or time loss detection.
    alarm_duration = this->_loss_time - Thread::get_hrtime();
    QUICLDDebug("Early retransmit timer or time loss detection will be set");
  } else if (this->_tlp_count < this->_MAX_TLPS) {
    // Tail Loss Probe
    if (this->_retransmittable_outstanding) {
      alarm_duration = 1.5 * this->_smoothed_rtt + this->_DELAYED_ACK_TIMEOUT;
    } else {
      alarm_duration = this->_MIN_TLP_TIMEOUT;
    }
    alarm_duration = std::max(alarm_duration, 2 * this->_smoothed_rtt);
    QUICLDDebug("TLP alarm will be set");
  } else {
    // RTO alarm
    alarm_duration = this->_smoothed_rtt + 4 * this->_rttvar;
    alarm_duration = std::max(alarm_duration, this->_MIN_RTO_TIMEOUT);
    alarm_duration = alarm_duration * (1 << this->_rto_count);
    QUICLDDebug("RTO alarm will be set");
  }

  if (this->_loss_detection_alarm_at) {
    this->_loss_detection_alarm_at = std::min(this->_loss_detection_alarm_at, Thread::get_hrtime() + alarm_duration);
  } else {
    this->_loss_detection_alarm_at = Thread::get_hrtime() + alarm_duration;
  }
  QUICLDDebug("Loss detection alarm has been set to %" PRId64, alarm_duration);

  if (!this->_loss_detection_alarm) {
    this->_loss_detection_alarm = eventProcessor.schedule_every(this, HRTIME_MSECONDS(25));
  }
}

std::set<QUICPacketNumber>
QUICLossDetector::_determine_newly_acked_packets(const QUICAckFrame &ack_frame)
{
  std::set<QUICPacketNumber> packets;
  QUICPacketNumber x = ack_frame.largest_acknowledged();
  for (uint64_t i = 0; i <= ack_frame.ack_block_section()->first_ack_block_length(); ++i) {
    packets.insert(x--);
  }
  for (auto &&block : *(ack_frame.ack_block_section())) {
    for (uint64_t i = 0; i <= block.gap(); ++i) {
      x--;
    }
    for (uint64_t i = 0; i <= block.length(); ++i) {
      packets.insert(x--);
    }
  }

  return packets;
}

void
QUICLossDetector::_retransmit_handshake_packets()
{
  SCOPED_MUTEX_LOCK(transmitter_lock, this->_transmitter->get_packet_transmitter_mutex().get(), this_ethread());
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  std::set<QUICPacketNumber> retransmitted_handshake_packets;

  for (auto &info : this->_sent_packets) {
    if (!info.second->handshake) {
      break;
    }
    retransmitted_handshake_packets.insert(info.first);
    this->_transmitter->retransmit_packet(*info.second->packet);
  }

  for (auto packet_number : retransmitted_handshake_packets) {
    this->_sent_packets.erase(packet_number);
    --this->_handshake_outstanding;
    --this->_retransmittable_outstanding;
  }
}

void
QUICLossDetector::_send_one_packet()
{
  SCOPED_MUTEX_LOCK(transmitter_lock, this->_transmitter->get_packet_transmitter_mutex().get(), this_ethread());
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  if (this->_transmitter->transmit_packet() < 1) {
    auto ite = this->_sent_packets.rbegin();
    if (ite != this->_sent_packets.rend()) {
      this->_transmitter->retransmit_packet(*ite->second->packet);
    }
  }
}

void
QUICLossDetector::_send_two_packets()
{
  SCOPED_MUTEX_LOCK(transmitter_lock, this->_transmitter->get_packet_transmitter_mutex().get(), this_ethread());
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  auto ite = this->_sent_packets.rbegin();
  if (ite != this->_sent_packets.rend()) {
    this->_transmitter->retransmit_packet(*ite->second->packet);
    ite++;
    if (ite != this->_sent_packets.rend()) {
      this->_transmitter->retransmit_packet(*ite->second->packet);
    }
  } else {
    this->_transmitter->transmit_packet();
  }
}
