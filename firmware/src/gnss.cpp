/*
 * Copyright (c) 2014 Zubax, zubax.com
 * Distributed under the MIT License, available in the file LICENSE.
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "gnss.hpp"
#include "board/ublox.hpp"
#include "node.hpp"

#include <uavcan/equipment/gnss/RTCMStream.hpp>
#include <uavcan/equipment/gnss/Fix.hpp>
#include <uavcan/equipment/gnss/Auxiliary.hpp>

#include <ch.hpp>
#include <zubax_chibios/sys/sys.h>
#include <zubax_chibios/config/config.hpp>
#include <zubax_chibios/watchdog/watchdog.hpp>
#include <unistd.h>

namespace gnss
{
namespace
{

SerialDriver* const serial_port = &SD2;

zubax_chibios::config::Param<float> param_gnss_fix_rate("gnss_fix_rate_hz", 10.0, 0.5, 15.0);
zubax_chibios::config::Param<float> param_gnss_aux_rate("gnss_aux_rate_hz", 1.0,  0.1, 1.0);

zubax_chibios::config::Param<unsigned> param_gnss_warn_min_fix_dimensions("gnss_warn_min_fix_dimensions", 0, 0, 3);
zubax_chibios::config::Param<unsigned> param_gnss_warn_min_sats_used("gnss_warn_min_sats_used", 0, 0, 20);


void publishFix(const ublox::Fix& data)
{
    static uavcan::equipment::gnss::Fix msg;
    msg = uavcan::equipment::gnss::Fix();

    // Timestamp - Network clock
    msg.timestamp = uavcan::UtcTime::fromUSec(data.ts.real_usec);

    // Timestamp - GNSS clock
    msg.gnss_timestamp = data.utc_valid ? uavcan::UtcTime::fromUSec(data.utc_usec) : uavcan::UtcTime();

    // Position
    msg.latitude_deg_1e8  = static_cast<std::int64_t>(data.lat * 1e8);
    msg.longitude_deg_1e8 = static_cast<std::int64_t>(data.lon * 1e8);

    msg.height_ellipsoid_mm = static_cast<std::int32_t>(data.height_wgs84 * 1e3F);
    msg.height_msl_mm       = static_cast<std::int32_t>(data.height_amsl * 1e3F);

    // Velocity
    std::copy(std::begin(data.ned_velocity), std::end(data.ned_velocity), std::begin(msg.ned_velocity));

    // Mode
    if (data.mode == ublox::Fix::Mode::Time)
    {
        msg.status = uavcan::equipment::gnss::Fix::STATUS_TIME_ONLY;
    }
    else if (data.mode == ublox::Fix::Mode::Fix2D)
    {
        msg.status = uavcan::equipment::gnss::Fix::STATUS_2D_FIX;
    }
    else if (data.mode == ublox::Fix::Mode::Fix3D)
    {
        msg.status = uavcan::equipment::gnss::Fix::STATUS_3D_FIX;
    }
    else
    {
        msg.status = uavcan::equipment::gnss::Fix::STATUS_NO_FIX;
    }

    // Uncertainty
    if (msg.status > uavcan::equipment::gnss::Fix::STATUS_TIME_ONLY)
    {
        msg.velocity_covariance.packSquareMatrix(data.velocity_covariance);
        msg.position_covariance.packSquareMatrix(data.position_covariance);
    }

    // Misc
    msg.sats_used = data.sats_used;
    msg.pdop = data.pdop;

    // Publishing
    node::Lock locker;
    static uavcan::Publisher<uavcan::equipment::gnss::Fix> pub(node::getNode());
    (void)pub.broadcast(msg);
}

void publishAuxiliary(const ublox::Fix& fix, const ublox::Auxiliary& aux)
{
    static uavcan::equipment::gnss::Auxiliary msg;
    msg = uavcan::equipment::gnss::Auxiliary();

    // DOP
    msg.gdop = aux.gdop;
    msg.hdop = aux.hdop;
    msg.pdop = aux.pdop;
    msg.tdop = aux.tdop;
    msg.vdop = aux.vdop;
    msg.ndop = aux.ndop;
    msg.edop = aux.edop;

    // Satellite stats
    msg.sats_used = fix.sats_used;
    msg.sats_visible = aux.num_sats;

    // Flags
    msg.differential_corrections_applied = (fix.flags & ublox::Fix::Flags::DifferentialSolution) != 0;

    // Publishing
    node::Lock locker;
    static uavcan::Publisher<uavcan::equipment::gnss::Auxiliary> pub(node::getNode());
    (void)pub.broadcast(msg);
}


class Platform : public ublox::IPlatform
{
public:
    void portWrite(const std::uint8_t* data, unsigned len) override
    {
        sdWrite(serial_port, data, len);
    }

    unsigned portRead(std::uint8_t* out_data, unsigned max_len, unsigned timeout_ms) override
    {
        return sdReadTimeout(serial_port, out_data, max_len, MS2ST(timeout_ms));
    }

    void portSetBaudRate(unsigned new_baudrate) override
    {
        static SerialConfig serial_cfg;
        serial_cfg = SerialConfig();
        serial_cfg.speed = new_baudrate;
        serial_cfg.cr2 = USART_CR2_STOP1_BITS;

        sdStop(serial_port);
        sdStart(serial_port, &serial_cfg);
    }

    std::uint64_t getMonotonicUSec() const override
    {
        return uavcan_stm32::clock::getMonotonic().toUSec();
    }

    std::uint64_t getRealUSec() const override
    {
        return uavcan_stm32::clock::getUtc().toUSec();
    }
};


class GnssThread : public chibios_rt::BaseStaticThread<3000>
{
    bool keep_going_ = true;

    unsigned warn_min_fix_dimensions_ = 0;
    unsigned warn_min_sats_used_ = 0;

    mutable zubax_chibios::watchdog::Timer watchdog_;
    mutable Platform platform_;
    mutable ublox::Driver driver_ = ublox::Driver(platform_);

    bool shouldKeepGoing() const
    {
        return keep_going_ && !node::hasPendingRestartRequest();
    }

    void pauseOneSec() const
    {
        watchdog_.reset();
        ::usleep(500000);
        watchdog_.reset();
        ::usleep(500000);
        watchdog_.reset();
    }

    void tryInit() const
    {
        auto cfg = ublox::Config();
        cfg.fix_rate_hz = param_gnss_fix_rate.get();
        cfg.aux_rate_hz = param_gnss_aux_rate.get();

        while (shouldKeepGoing() && !driver_.configure(cfg, watchdog_))
        {
            lowsyslog("GNSS driver init failed\n");
            pauseOneSec();
        }
    }

    void tryRun() const
    {
        do
        {
            watchdog_.reset();
            driver_.spin(100);
        }
        while (shouldKeepGoing() && driver_.areRatesValid());
    }

    void handleFix(const ublox::Fix& fix) const
    {
        // Publish the new GNSS solution onto the bus
        publishFix(fix);

        // Update component status
        const bool warn = (static_cast<unsigned>(fix.mode) < warn_min_fix_dimensions_) ||
                          (fix.sats_used < warn_min_sats_used_);
        auto stat = warn ? uavcan::protocol::NodeStatus::STATUS_WARNING : uavcan::protocol::NodeStatus::STATUS_OK;
        node::setComponentStatus(node::ComponentID::Gnss, stat);

        // Adjust the local time (locked to the global UTC)
        if (fix.utc_valid)
        {
            auto adj = uavcan::UtcTime::fromUSec(fix.utc_usec) - uavcan::UtcTime::fromUSec(fix.ts.real_usec);
            node::adjustUtcTimeFromLocalSource(adj);
        }
    }

public:
    msg_t main() override
    {
        watchdog_.startMSec(1000);
        setName("gnss");

        warn_min_fix_dimensions_ = param_gnss_warn_min_fix_dimensions.get();
        warn_min_sats_used_ = param_gnss_warn_min_sats_used.get();

        driver_.on_fix = std::bind(&GnssThread::handleFix, this, std::placeholders::_1);
        driver_.on_aux = [this](const ublox::Auxiliary& aux) { publishAuxiliary(driver_.getFix(), aux); };

        pauseOneSec();  // Waiting for the receiver to boot

        while (shouldKeepGoing())
        {
            pauseOneSec();
            lowsyslog("GNSS init...\n");
            tryInit();
            tryRun();
            node::setComponentStatus(node::ComponentID::Gnss, uavcan::protocol::NodeStatus::STATUS_CRITICAL);
        }

        lowsyslog("GNSS driver terminated\n");
        while (true)
        {
            pauseOneSec();   // We don't exit the thread because GNSS driver termination is not fatal
        }
        return msg_t();
    }

    void stop() override
    {
        keep_going_ = false;
    }
} gnss_thread;

}

void init()
{
    (void)gnss_thread.start(HIGHPRIO);
}

void stop()
{
    gnss_thread.stop();
}

SerialDriver& getSerialPort()
{
    return *serial_port;
}

}
