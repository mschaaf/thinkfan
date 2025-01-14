/********************************************************************
 * drivers.cpp: Interface to the kernel drivers.
 * (C) 2015, Victor Mataré
 *
 * this file is part of thinkfan. See thinkfan.c for further information.
 *
 * thinkfan is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * thinkfan is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with thinkfan.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ******************************************************************/

#include "fans.h"
#include "error.h"
#include "message.h"
#include "config.h"

#include <fstream>
#include <cstring>
#include <thread>
#include <typeinfo>

#ifdef USE_NVML
#include <dlfcn.h>
#endif

namespace thinkfan {

/*----------------------------------------------------------------------------
| FanDriver: Superclass of TpFanDriver and HwmonFanDriver. Can set the speed |
| on its own since an implementation-specific string representation is       |
| provided by its subclasses.                                                |
----------------------------------------------------------------------------*/

FanDriver::FanDriver(const std::string &path, const unsigned int watchdog_timeout)
: path_(path),
  watchdog_(watchdog_timeout),
  depulse_(0)
{}

void FanDriver::set_speed(const string &level)
{
	std::ofstream f_out(path_);
	if(!(f_out << level << std::flush)) {
		int err = errno;
		if (err == EPERM)
			throw SystemError(MSG_FAN_EPERM(path_));
		else
			throw IOerror(MSG_FAN_CTRL(level, path_), err);
	}
	current_speed_ = level;
}


bool FanDriver::operator == (const FanDriver &other) const
{
	return typeid(*this) == typeid(other)
			&& this->path_ == other.path_
			&& this->depulse_ == other.depulse_
			&& this->watchdog_ == other.watchdog_;
}

const string &FanDriver::current_speed() const
{ return current_speed_; }


/*----------------------------------------------------------------------------
| TpFanDriver: Driver for fan control via thinkpad_acpi, typically in        |
| /proc/acpi/ibm/fan. Supports fan watchdog and depulsing (an alleged remedy |
| for noise oscillation with old & worn-out fans).                           |
----------------------------------------------------------------------------*/

TpFanDriver::TpFanDriver(const std::string &path)
: FanDriver(path, 120)
{}

TpFanDriver::~TpFanDriver() noexcept(false)
{
	std::ofstream f(path_);
	if (!(f.is_open() && f.good())) {
		log(TF_ERR) << MSG_FAN_RESET(path_) << strerror(errno) << flush;
		return;
	}

	if (!initial_state_.empty()) {
		log(TF_DBG) << path_ << ": Restoring initial state: " << initial_state_ << "." << flush;
		if (!(f << "level " << initial_state_ << std::flush))
			log(TF_ERR) << MSG_FAN_RESET(path_) << strerror(errno) << flush;
	}
}


void TpFanDriver::set_watchdog(const unsigned int timeout)
{ watchdog_ = std::chrono::duration<unsigned int>(timeout); }


void TpFanDriver::set_depulse(float duration)
{ depulse_ = std::chrono::duration<float>(duration); }


void TpFanDriver::set_speed(const Level &level)
{
	FanDriver::set_speed(level.str());
	last_watchdog_ping_ = std::chrono::system_clock::now();
}


void TpFanDriver::ping_watchdog_and_depulse(const Level &level)
{
	if (depulse_ > std::chrono::milliseconds(0)) {
		FanDriver::set_speed("level disengaged");
		std::this_thread::sleep_for(depulse_);
		set_speed(level);
	}
	else if (last_watchdog_ping_ + watchdog_ - sleeptime <= std::chrono::system_clock::now()) {
		log(TF_DBG) << "Watchdog ping" << flush;
		set_speed(level);
	}
}


void TpFanDriver::init()
{
	bool ctrl_supported = false;
	std::fstream f(path_);
	if (!(f.is_open() && f.good()))
		throw IOerror(MSG_FAN_INIT(path_), errno);

	std::string line;
	line.resize(256);

	while (f.getline(&*line.begin(), 255)) {
		if (initial_state_.empty() && line.rfind("level:") != string::npos) {
			// remember initial level, restore it in d'tor
			string::size_type offs = line.find_last_of(" \t") + 1;
			if (offs != string::npos) {
				// Cut of at bogus \000 char that may occur before EOL
				initial_state_ = line.substr(offs, line.find_first_of('\000') - offs);
			}
			log(TF_DBG) << path_ << ": Saved initial state: " << initial_state_ << "." << flush;
		}
		else if (line.rfind("commands:") != std::string::npos && line.rfind("level <level>") != std::string::npos) {
			ctrl_supported = true;
		}
	}

	if (!ctrl_supported)
		throw SystemError(MSG_FAN_MODOPTS);

	if (initial_state_.empty())
		throw SystemError(MSG_FAN_INIT(path_) + "Failed to read initial state.");

	f.close();
	f.open(path_);

	if (!(f << "watchdog " << watchdog_.count() << std::flush))
		throw IOerror(MSG_FAN_INIT(path_), errno);
}


/*----------------------------------------------------------------------------
| HwmonFanDriver: Driver for PWM fans, typically somewhere in sysfs.         |
----------------------------------------------------------------------------*/

HwmonFanDriver::HwmonFanDriver(const std::string &path)
: FanDriver(path, 0)
{}


HwmonFanDriver::~HwmonFanDriver() noexcept(false)
{
	std::ofstream f(path_ + "_enable");
	if (!(f.is_open() && f.good())) {
		log(TF_ERR) << MSG_FAN_RESET(path_) << strerror(errno) << flush;
		return;
	}

	if (!initial_state_.empty()) {
		log(TF_DBG) << path_ << ": Restoring initial state: " << initial_state_ << "." << flush;
		if (!(f << initial_state_ << std::flush))
			log(TF_ERR) << MSG_FAN_RESET(path_) << strerror(errno) << flush;
	}
}


void HwmonFanDriver::init()
{
	std::fstream f(path_ + "_enable");
	if (!(f.is_open() && f.good()))
		throw IOerror(MSG_FAN_INIT(path_), errno);

	if (initial_state_.empty()) {
		std::string line;
		line.resize(64);
		if (!f.getline(&*line.begin(), 63))
			throw IOerror(MSG_FAN_INIT(path_), errno);
		initial_state_ = line;
		log(TF_DBG) << path_ << ": Saved initial state: " << initial_state_ << "." << flush;
	}

	if (!(f << "1" << std::flush))
		throw IOerror(MSG_FAN_INIT(path_), errno);
}


void HwmonFanDriver::set_speed(const Level &level)
{
	try {
		FanDriver::set_speed(std::to_string(level.num()));
	} catch (IOerror &e) {
		if (e.code() == EINVAL) {
			// This happens when the hwmon kernel driver is reset to automatic control
			// e.g. after the system has woken up from suspend.
			// In that case, we need to re-initialize and try once more.
			init();
			FanDriver::set_speed(std::to_string(level.num()));
			log(TF_WRN) << path_ << ": WARNING: Userspace fan control had to be automatically re-initialized." << flush;
#if defined(HAVE_SYSTEMD)
			log(TF_WRN) << "This should have been taken care of when enabling the thinkfan systemd service." << flush
			            << "If thinkfan.service is enabled, the following services should also have be enabled as a dependency:" << flush
			            << "thinkfan-hibernate.service, thinkfan-hybrid-suspend.service and thinkfan-suspend.service" << flush;
#else
			log(TF_WRN) << "Please arrange for a SIGUSR2 to be sent to thinkfan after resuming from suspend." << flush;
#endif
		} else {
			throw;
		}
	}
}

}
