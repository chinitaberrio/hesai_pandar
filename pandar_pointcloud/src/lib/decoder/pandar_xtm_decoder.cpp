#include "pandar_pointcloud/decoder/pandar_xtm_decoder.hpp"
#include "pandar_pointcloud/decoder/pandar_xtm.hpp"

namespace
{
static inline double deg2rad(double degrees)
{
  return degrees * M_PI / 180.0;
}
}

namespace pandar_pointcloud
{
namespace pandar_xtm
{
PandarXTMDecoder::PandarXTMDecoder(Calibration& calibration, float scan_phase, double dual_return_distance_threshold, ReturnMode return_mode)
{
  for(int unit = 0; unit < UNIT_NUM; ++unit){
    firing_offset_[unit] = 2.856f * unit + 0.368f;
  }

  block_offset_single_ = {
      5.632f - 50.0f * 5.0f,
      5.632f - 50.0f * 4.0f,
      5.632f - 50.0f * 3.0f,
      5.632f - 50.0f * 2.0f,
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f
  };
  block_offset_dual_ = {
      5.632f - 50.0f * 2.0f,
      5.632f - 50.0f * 2.0f,
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f
  };
  block_offset_triple_ = {
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 1.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f,
      5.632f - 50.0f * 0.0f
  };

  // TODO: add calibration data validation
  // if(calibration.elev_angle_map.size() != num_lasers_){
  //   // calibration data is not valid!
  // }
  for (size_t laser = 0; laser < UNIT_NUM; ++laser) {
    elev_angle_[laser] = calibration.elev_angle_map[laser];
    azimuth_offset_[laser] = calibration.azimuth_offset_map[laser];
  }

  scan_phase_ = static_cast<uint16_t>(scan_phase * 100.0f);
  return_mode_ = return_mode;

  last_phase_ = 0;
  has_scanned_ = false;

  scan_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
}

bool PandarXTMDecoder::hasScanned()
{
  return has_scanned_;
}

PointcloudXYZIRADT PandarXTMDecoder::getPointcloud()
{
  return scan_pc_;
}

void PandarXTMDecoder::unpack(const pandar_msgs::PandarPacket& raw_packet)
{
  if (!parsePacket(raw_packet)) {
    return;
  }

  if (has_scanned_) {
    scan_pc_ = overflow_pc_;
    overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
    has_scanned_ = false;
  }

  int step = 1;
  switch (packet_.return_mode) {
    case FIRST_RETURN:
    case STRONGEST_RETURN:
    case LAST_RETURN:
      step = 1;
      break;
    case DUAL_RETURN:
      step = 2;
      break;
    case TRIPLE_RETURN:
      step = 3;
      break;
  }

  for (int block_id = 0; block_id < BLOCK_NUM; block_id += step) {
    PointcloudXYZIRADT block_pc;
    if (step == 1) {
      block_pc = convert(block_id);
    }
    if (step == 2) {
      block_pc = convert_dual(block_id);
    }
    if (step == 3) {
      block_pc = convert_triple(block_id);
    }
    int current_phase = (static_cast<int>(packet_.blocks[block_id].azimuth) - scan_phase_ + 36000) % 36000;
    if (current_phase > last_phase_ && !has_scanned_) {
      *scan_pc_ += *block_pc;
    }
    else {
      *overflow_pc_ += *block_pc;
      has_scanned_ = true;
    }
    last_phase_ = current_phase;
  }
}

PointcloudXYZIRADT PandarXTMDecoder::convert(const int block_id)
{
  PointcloudXYZIRADT block_pc(new pcl::PointCloud<PointXYZIRADT>);

  // double unix_second = raw_packet.header.stamp.toSec() // system-time (packet receive time)
  double unix_second = static_cast<double>(timegm(&packet_.t));  // sensor-time (ppt/gps)

  const auto& block = packet_.blocks[block_id];
  for (size_t unit_id = 0; unit_id < UNIT_NUM; ++unit_id) {
    PointXYZIRADT point;
    const auto& unit = block.units[unit_id];
    // skip invalid points
    if (unit.distance <= 0.1 || unit.distance > 200.0) {
      continue;
    }
    double xyDistance = unit.distance * cosf(deg2rad(elev_angle_[unit_id]));

    point.x = static_cast<float>(
        xyDistance * sinf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
    point.y = static_cast<float>(
        xyDistance * cosf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
    point.z = static_cast<float>(unit.distance * sinf(deg2rad(elev_angle_[unit_id])));

    point.intensity = unit.intensity;
    point.distance = unit.distance;
    point.ring = unit_id;
    point.azimuth = block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);

    point.time_stamp = unix_second + (static_cast<double>(packet_.usec)) / 1000000.0;

    point.time_stamp += (static_cast<double>(block_offset_single_[block_id] + firing_offset_[unit_id]) / 1000000.0f);

    block_pc->push_back(point);
  }
  return block_pc;
}

PointcloudXYZIRADT PandarXTMDecoder::convert_dual(const int block_id)
{
  PointcloudXYZIRADT block_pc(new pcl::PointCloud<PointXYZIRADT>);

  // double unix_second = raw_packet.header.stamp.toSec() // system-time (packet receive time)
  double unix_second = static_cast<double>(timegm(&packet_.t));  // sensor-time (ppt/gps)

  auto head = block_id + ((return_mode_ == ReturnMode::FIRST) ? 1 : 0);
  auto tail = block_id + ((return_mode_ == ReturnMode::LAST) ? 1 : 2);

  for (size_t unit_id = 0; unit_id < UNIT_NUM; ++unit_id) {
    for (int i = head; i < tail; ++i) {
      PointXYZIRADT point;
      const auto& block = packet_.blocks[i];
      const auto& unit = block.units[unit_id];
      // skip invalid points
      if (unit.distance <= 0.1 || unit.distance > 200.0) {
        continue;
      }
      double xyDistance = unit.distance * cosf(deg2rad(elev_angle_[unit_id]));

      point.x = static_cast<float>(
          xyDistance * sinf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
      point.y = static_cast<float>(
          xyDistance * cosf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
      point.z = static_cast<float>(unit.distance * sinf(deg2rad(elev_angle_[unit_id])));

      point.intensity = unit.intensity;
      point.distance = unit.distance;
      point.ring = unit_id;
      point.azimuth = block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);

      point.time_stamp = unix_second + (static_cast<double>(packet_.usec)) / 1000000.0;

      point.time_stamp += (static_cast<double>(block_offset_dual_[block_id] + firing_offset_[unit_id]) / 1000000.0f);

      block_pc->push_back(point);
    }
  }
  return block_pc;
}

PointcloudXYZIRADT PandarXTMDecoder::convert_triple(const int block_id)
{
  PointcloudXYZIRADT block_pc(new pcl::PointCloud<PointXYZIRADT>);

  // double unix_second = raw_packet.header.stamp.toSec() // system-time (packet receive time)
  double unix_second = static_cast<double>(timegm(&packet_.t));  // sensor-time (ppt/gps)

  auto head = block_id + ((return_mode_ == ReturnMode::FIRST) ? 1 : 0);
  auto tail = block_id + ((return_mode_ == ReturnMode::LAST) ? 1 : 2);

  for (size_t unit_id = 0; unit_id < UNIT_NUM; ++unit_id) {
    for (int i = head; i < tail; ++i) {
      PointXYZIRADT point;
      const auto& block = packet_.blocks[i];
      const auto& unit = block.units[unit_id];
      // skip invalid points
      if (unit.distance <= 0.1 || unit.distance > 200.0) {
        continue;
      }
      double xyDistance = unit.distance * cosf(deg2rad(elev_angle_[unit_id]));

      point.x = static_cast<float>(
          xyDistance * sinf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
      point.y = static_cast<float>(
          xyDistance * cosf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
      point.z = static_cast<float>(unit.distance * sinf(deg2rad(elev_angle_[unit_id])));

      point.intensity = unit.intensity;
      point.distance = unit.distance;
      point.ring = unit_id;
      point.azimuth = block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);

      point.time_stamp = unix_second + (static_cast<double>(packet_.usec)) / 1000000.0;

      point.time_stamp += (static_cast<double>(block_offset_triple_[block_id] + firing_offset_[unit_id]) / 1000000.0f);

      block_pc->push_back(point);
    }
  }
  return block_pc;
}

bool PandarXTMDecoder::parsePacket(const pandar_msgs::PandarPacket& raw_packet)
{
  if (raw_packet.size != PACKET_SIZE) {
    return false;
  }
  const uint8_t* buf = &raw_packet.data[0];

  size_t index = 0;
  // Parse 12 Bytes Header
  packet_.header.sob = (buf[index] & 0xff) << 8 | ((buf[index + 1] & 0xff));
  packet_.header.chProtocolMajor = buf[index + 2] & 0xff;
  packet_.header.chProtocolMinor = buf[index + 3] & 0xff;
  packet_.header.chLaserNumber = buf[index + 6] & 0xff;
  packet_.header.chBlockNumber = buf[index + 7] & 0xff;
  packet_.header.chReturnType = buf[index + 8] & 0xff;
  packet_.header.chDisUnit = buf[index + 9] & 0xff;
  index += HEAD_SIZE;

  if (packet_.header.sob != 0xEEFF) {
    // Error Start of Packet!
    return false;
  }

  for (size_t block = 0; block < packet_.header.chBlockNumber; block++) {
    packet_.blocks[block].azimuth = (buf[index] & 0xff) | ((buf[index + 1] & 0xff) << 8);
    index += BLOCK_HEADER_AZIMUTH;

    for (int unit = 0; unit < packet_.header.chLaserNumber; unit++) {
      unsigned int unRange = (buf[index] & 0xff) | ((buf[index + 1] & 0xff) << 8);

      packet_.blocks[block].units[unit].distance =
          (static_cast<double>(unRange * packet_.header.chDisUnit)) / (double)1000;
      packet_.blocks[block].units[unit].intensity = (buf[index + 2] & 0xff);
      packet_.blocks[block].units[unit].confidence = (buf[index + 3] & 0xff);
      index += UNIT_SIZE;
    }
  }

  index += RESERVED_SIZE;  // skip reserved bytes
  packet_.return_mode = buf[index] & 0xff;

  index += RETURN_SIZE;
  index += ENGINE_VELOCITY;

  packet_.t.tm_year = (buf[index + 0] & 0xff) + 100;
  packet_.t.tm_mon = (buf[index + 1] & 0xff) - 1;
  packet_.t.tm_mday = buf[index + 2] & 0xff;
  packet_.t.tm_hour = buf[index + 3] & 0xff;
  packet_.t.tm_min = buf[index + 4] & 0xff;
  packet_.t.tm_sec = buf[index + 5] & 0xff;
  packet_.t.tm_isdst = 0;
  // in case of time error
  if (packet_.t.tm_year >= 200) {
    packet_.t.tm_year -= 100;
  }

  index += UTC_SIZE;

  packet_.usec = (buf[index] & 0xff) | (buf[index + 1] & 0xff) << 8 | ((buf[index + 2] & 0xff) << 16) |
                 ((buf[index + 3] & 0xff) << 24);
  index += TIMESTAMP_SIZE;
  index += FACTORY_SIZE;

  return true;
}
}
}