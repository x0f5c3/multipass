/*
 * Copyright (C) Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "lxd_virtual_machine.h"
#include "lxd_mount_handler.h"
#include "lxd_request.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <multipass/exceptions/local_socket_connection_exception.h>
#include <multipass/exceptions/snap_environment_exception.h>
#include <multipass/exceptions/start_exception.h>
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/memory_size.h>
#include <multipass/network_access_manager.h>
#include <multipass/snap_utils.h>
#include <multipass/top_catch_all.h>
#include <multipass/utils.h>
#include <multipass/virtual_machine_description.h>
#include <multipass/vm_status_monitor.h>

#include <shared/shared_backend_utils.h>

#include <chrono>
#include <thread>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpu = multipass::utils;

using namespace std::literals::chrono_literals;

namespace
{
auto instance_state_for(const QString& name, mp::NetworkAccessManager* manager, const QUrl& url)
{
    auto json_reply = lxd_request(manager, "GET", url);
    auto metadata = json_reply["metadata"].toObject();
    mpl::log(mpl::Level::trace, name.toStdString(),
             fmt::format("Got LXD container state: {} is {}", name, metadata["status"].toString()));

    switch (metadata["status_code"].toInt(-1))
    {
    case 101: // Started
    case 103: // Running
    case 107: // Stopping
    case 111: // Thawed
        return mp::VirtualMachine::State::running;
    case 102: // Stopped
        return mp::VirtualMachine::State::stopped;
    case 106: // Starting
        return mp::VirtualMachine::State::starting;
    case 109: // Freezing
        return mp::VirtualMachine::State::suspending;
    case 110: // Frozen
        return mp::VirtualMachine::State::suspended;
    case 104: // Cancelling
    case 108: // Aborting
    case 112: // Error
        return mp::VirtualMachine::State::unknown;
    default:
        mpl::log(mpl::Level::error, name.toStdString(),
                 fmt::format("Got unexpected LXD state: {} ({})", metadata["status"].toString(),
                             metadata["status_code"].toInt()));
        return mp::VirtualMachine::State::unknown;
    }
}

std::optional<mp::IPAddress> get_ip_for(const QString& mac_addr, mp::NetworkAccessManager* manager, const QUrl& url)
{
    const auto json_leases = lxd_request(manager, "GET", url);
    const auto leases = json_leases["metadata"].toArray();

    for (const auto lease : leases)
    {
        if (lease.toObject()["hwaddr"].toString() == mac_addr)
        {
            try
            {
                return std::optional<mp::IPAddress>{lease.toObject()["address"].toString().toStdString()};
            }
            catch (const std::invalid_argument&)
            {
                continue;
            }
        }
    }

    return std::nullopt;
}

QJsonObject generate_base_vm_config(const multipass::VirtualMachineDescription& desc)
{
    QJsonObject config{{"limits.cpu", QString::number(desc.num_cores)},
                       {"limits.memory", QString::number(desc.mem_size.in_bytes())},
                       {"security.secureboot", "false"}};

    if (!desc.meta_data_config.IsNull())
        config["user.meta-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.meta_data_config));

    if (!desc.vendor_data_config.IsNull())
        config["user.vendor-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.vendor_data_config));

    if (!desc.user_data_config.IsNull())
        config["user.user-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.user_data_config));

    if (!desc.network_data_config.IsNull())
        config["user.network-config"] = QString::fromStdString(mpu::emit_cloud_config(desc.network_data_config));

    return config;
}

QJsonObject generate_devices_config(const multipass::VirtualMachineDescription& desc, const QString& default_mac_addr,
                                    const QString& storage_pool)
{
    QJsonObject devices{{"config", QJsonObject{{"source", "cloud-init:config"}, {"type", "disk"}}},
                        {"root", QJsonObject{{"path", "/"},
                                             {"pool", storage_pool},
                                             {"size", QString::number(desc.disk_space.in_bytes())},
                                             {"type", "disk"}}},
                        {"eth0", QJsonObject{{"name", "eth0"},
                                             {"nictype", "bridged"},
                                             {"parent", "mpbr0"},
                                             {"type", "nic"},
                                             {"hwaddr", default_mac_addr}}}};

    for (auto i = 0u; i < desc.extra_interfaces.size();)
    {
        const auto& net = desc.extra_interfaces[i];
        auto net_name = QStringLiteral("eth%1").arg(++i);
        devices.insert(net_name, QJsonObject{{"name", net_name},
                                             {"nictype", "bridged"},
                                             {"parent", QString::fromStdString(net.id)},
                                             {"type", "nic"},
                                             {"hwaddr", QString::fromStdString(net.mac_address)}});
    }

    return devices;
}

} // namespace

mp::LXDVirtualMachine::LXDVirtualMachine(const VirtualMachineDescription& desc, VMStatusMonitor& monitor,
                                         NetworkAccessManager* manager, const QUrl& base_url,
                                         const QString& bridge_name, const QString& storage_pool)
    : BaseVirtualMachine{desc.vm_name},
      name{QString::fromStdString(desc.vm_name)},
      username{desc.ssh_username},
      monitor{&monitor},
      manager{manager},
      base_url{base_url},
      bridge_name{bridge_name},
      mac_addr{QString::fromStdString(desc.default_mac_address)},
      storage_pool{storage_pool}
{
    try
    {
        current_state();
    }
    catch (const LXDNotFoundException& e)
    {
        mpl::log(mpl::Level::debug, name.toStdString(),
                 fmt::format("Creating instance with image id: {}", desc.image.id));

        QJsonObject virtual_machine{
            {"name", name},
            {"config", generate_base_vm_config(desc)},
            {"devices", generate_devices_config(desc, mac_addr, storage_pool)},
            {"source", QJsonObject{{"type", "image"}, {"fingerprint", QString::fromStdString(desc.image.id)}}}};

        auto json_reply = lxd_request(manager, "POST", QUrl(QString("%1/virtual-machines").arg(base_url.toString())),
                                      virtual_machine);

        // TODO: Need a way to pass in the daemon timeout and make in general for all back ends
        lxd_wait(manager, base_url, json_reply, 600000);

        current_state();
    }
}

mp::LXDVirtualMachine::~LXDVirtualMachine()
{
    update_shutdown_status = false;

    mp::top_catch_all(vm_name, [this]() {
        try
        {
            current_state();

            if (state == State::running)
            {
                try
                {
                    if (!QFileInfo::exists(mp::utils::snap_common_dir() + "/snap_refresh"))
                        stop();
                }
                catch (const mp::SnapEnvironmentException&)
                {
                    stop();
                }
            }
            else
            {
                update_state();
            }
        }
        catch (const LXDNotFoundException& e)
        {
            mpl::log(mpl::Level::debug, vm_name, fmt::format("LXD object not found"));
        }
    });
}

void mp::LXDVirtualMachine::start()
{
    if (state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Resuming from a suspended state"));
        request_state("unfreeze");
    }
    else
    {
        request_state("start");
    }

    state = State::starting;
    update_state();
}

void mp::LXDVirtualMachine::stop()
{
    std::unique_lock<decltype(state_mutex)> lock{state_mutex};
    auto present_state = current_state();

    if (present_state == State::stopped)
    {
        mpl::log(mpl::Level::debug, vm_name, "Ignoring stop request since instance is already stopped");
        return;
    }

    if (present_state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Ignoring shutdown issued while suspended"));
        return;
    }

    request_state("stop");

    state = State::stopped;

    if (present_state == State::starting)
    {
        state_wait.wait(lock, [this] { return shutdown_while_starting; });
    }

    port = std::nullopt;

    if (update_shutdown_status)
        update_state();
}

void mp::LXDVirtualMachine::shutdown()
{
    stop();
}

void mp::LXDVirtualMachine::suspend()
{
    throw std::runtime_error("suspend is currently not supported");
}

mp::VirtualMachine::State mp::LXDVirtualMachine::current_state()
{
    try
    {
        auto present_state = instance_state_for(name, manager, state_url());

        if ((state == State::delayed_shutdown || state == State::starting) && present_state == State::running)
            return state;

        state = present_state;
    }
    catch (const LocalSocketConnectionException& e)
    {
        mpl::log(mpl::Level::warning, vm_name, e.what());
        state = State::unknown;
    }

    return state;
}

int mp::LXDVirtualMachine::ssh_port()
{
    return 22;
}

void mp::LXDVirtualMachine::ensure_vm_is_running()
{
    ensure_vm_is_running(20s);
}

void mp::LXDVirtualMachine::ensure_vm_is_running(const std::chrono::milliseconds& timeout)
{
    auto is_vm_running = [this, timeout] {
        if (current_state() != State::stopped)
        {
            return true;
        }

        // Sleep to see if LXD is just rebooting the instance
        std::this_thread::sleep_for(timeout);

        if (current_state() != State::stopped)
        {
            state = State::starting;
            return true;
        }

        return false;
    };

    mp::backend::ensure_vm_is_running_for(this, is_vm_running, "Instance shutdown during start");
}

void mp::LXDVirtualMachine::update_state()
{
    monitor->persist_state_for(vm_name, state);
}

std::string mp::LXDVirtualMachine::ssh_hostname(std::chrono::milliseconds timeout)
{
    auto get_ip = [this]() -> std::optional<IPAddress> { return get_ip_for(mac_addr, manager, network_leases_url()); };

    return mp::backend::ip_address_for(this, get_ip, timeout);
}

std::string mp::LXDVirtualMachine::ssh_username()
{
    return username;
}

std::string mp::LXDVirtualMachine::management_ipv4()
{
    if (!management_ip)
    {
        management_ip = get_ip_for(mac_addr, manager, network_leases_url());
        if (!management_ip)
        {
            mpl::log(mpl::Level::trace, name.toStdString(), "IP address not found.");
            return "UNKNOWN";
        }
    }

    return management_ip.value().as_string();
}

std::string mp::LXDVirtualMachine::ipv6()
{
    return {};
}

void mp::LXDVirtualMachine::wait_until_ssh_up(std::chrono::milliseconds timeout)
{
    mpu::wait_until_ssh_up(this, timeout, [this] { ensure_vm_is_running(); });
}

const QUrl mp::LXDVirtualMachine::url()
{
    return QString("%1/virtual-machines/%2").arg(base_url.toString()).arg(name);
}

const QUrl mp::LXDVirtualMachine::state_url()
{
    return url().toString() + "/state";
}

const QUrl mp::LXDVirtualMachine::network_leases_url()
{
    return base_url.toString() + "/networks/" + bridge_name + "/leases";
}

void mp::LXDVirtualMachine::request_state(const QString& new_state)
{
    const QJsonObject state_json{{"action", new_state}};

    auto state_task = lxd_request(manager, "PUT", state_url(), state_json, 5000);

    try
    {
        lxd_wait(manager, base_url, state_task, 60000);
    }
    catch (const LXDNotFoundException&)
    {
        // Implies the task doesn't exist, move on...
    }
}

void multipass::LXDVirtualMachine::update_cpus(int num_cores)
{
    assert(num_cores > 0);
    assert(manager);

    /*
     * similar to:
     * $ curl -s -w "%{http_code}" -X PATCH -H "Content-Type: application/json" \
     *        -d '{"config": {"limits.cpu": "3"}}' \
     *        --unix-socket /var/snap/lxd/common/lxd/unix.socket \
     *        lxd/1.0/virtual-machines/asdf?project=multipass
     */
    QJsonObject patch_json{{"config", QJsonObject{{"limits.cpu", QString::number(num_cores)}}}};
    auto reply = lxd_request(manager, "PATCH", url(), patch_json);
}

void mp::LXDVirtualMachine::resize_memory(const MemorySize& new_size)
{
    assert(new_size.in_bytes() > 0);
    assert(manager);

    /*
     * similar to:
     * $ curl -s -w "%{http_code}" -X PATCH -H "Content-Type: application/json" \
     *        -d '{"config": {"limits.memory": "1572864000"}}' \
     *        --unix-socket /var/snap/lxd/common/lxd/unix.socket \
     *        lxd/1.0/virtual-machines/asdf?project=multipass
     */
    QJsonObject patch_json{{"config", QJsonObject{{"limits.memory", QString::number(new_size.in_bytes())}}}};
    auto reply = lxd_request(manager, "PATCH", url(), patch_json);
}

void mp::LXDVirtualMachine::resize_disk(const MemorySize& new_size)
{
    assert(new_size.in_bytes() > 0);
    assert(manager);

    /*
     * similar to:
     * $ curl -s -w "%{http_code}\n" -X PATCH -H "Content-Type: application/json" \
     *        -d '{"devices": {"root": {"size": "10737418245B"}}}' \
     *        --unix-socket /var/snap/lxd/common/lxd/unix.socket \
     *        lxd/1.0/virtual-machines/asdf?project=multipass
     */
    QJsonObject root_json{
        {"path", "/"}, {"pool", storage_pool}, {"size", QString::number(new_size.in_bytes())}, {"type", "disk"}};
    QJsonObject patch_json{{"devices", QJsonObject{{"root", root_json}}}};
    lxd_request(manager, "PATCH", url(), patch_json);
}

std::unique_ptr<multipass::MountHandler>
mp::LXDVirtualMachine::make_native_mount_handler(const SSHKeyProvider* ssh_key_provider, const std::string& target,
                                                 const VMMount& mount)
{
    if (!mount.gid_mappings.empty() || !mount.uid_mappings.empty())
    {
        throw std::runtime_error("lxd native mount does not accept gid or uid.");
    }

    return std::make_unique<LXDMountHandler>(manager, this, ssh_key_provider, target, mount);
}
