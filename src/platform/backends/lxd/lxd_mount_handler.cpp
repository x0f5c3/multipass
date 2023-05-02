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

#include "lxd_mount_handler.h"
#include "lxd_request.h"

namespace
{
constexpr std::string_view category = "lxd-mount-handler";
constexpr int length_of_unique_id_without_prefix = 25;
constexpr int timeout_millisecond = 300000;
} // namespace

namespace multipass
{
LXDMountHandler::LXDMountHandler(mp::NetworkAccessManager* network_manager, LXDVirtualMachine* lxd_virtual_machine,
                                 const SSHKeyProvider* ssh_key_provider, const std::string& target_path,
                                 const VMMount& mount)
    : MountHandler{lxd_virtual_machine, ssh_key_provider, target_path, mount.source_path},
      network_manager_{network_manager},
      lxd_instance_endpoint{
          QString("%1/instances/%2").arg(lxd_socket_url.toString()).arg(lxd_virtual_machine->vm_name.c_str())},
      // 27 (25 + 2(d_)) letters is the maximal deivce name length that lxd can accepte
      device_name_{
          mp::utils::make_uuid(target_path).left(length_of_unique_id_without_prefix).prepend("d_").toStdString()}
{
    const VirtualMachine::State state = lxd_virtual_machine->current_state();
    if (state != VirtualMachine::State::off && state != VirtualMachine::State::stopped)
    {
        throw std::runtime_error("Please stop the instance " + lxd_virtual_machine->vm_name +
                                 " before mount it natively.");
    }

    const std::lock_guard active_lock{active_mutex};
    mpl::log(mpl::Level::info, std::string(category),
             fmt::format("initializing native mount {} => {} in '{}'", source, target, lxd_virtual_machine->vm_name));
    lxd_device_add();
}

void LXDMountHandler::start_impl(ServerVariant /**/, std::chrono::milliseconds /**/)
{
}

void LXDMountHandler::stop_impl(bool force)
{
}

LXDMountHandler::~LXDMountHandler()
{
    const std::lock_guard active_lock{active_mutex};
    mpl::log(mpl::Level::info, std::string(category),
             fmt::format("Stopping native mount \"{}\" in instance '{}'", target, vm->vm_name));
    lxd_device_remove();
}

void LXDMountHandler::lxd_device_remove()
{
    const QJsonObject instance_info = lxd_request(network_manager_, "GET", lxd_instance_endpoint);
    QJsonObject instance_info_metadata = instance_info["metadata"].toObject();
    QJsonObject device_list = instance_info_metadata["devices"].toObject();

    device_list.remove(device_name_.c_str());
    instance_info_metadata["devices"] = device_list;

    const QJsonObject json_reply = lxd_request(network_manager_, "PUT", lxd_instance_endpoint, instance_info_metadata);
    lxd_wait(network_manager_, multipass::lxd_socket_url, json_reply, timeout_millisecond);
}

void LXDMountHandler::lxd_device_add()
{
    const QJsonObject instance_info = lxd_request(network_manager_, "GET", lxd_instance_endpoint);
    QJsonObject instance_info_metadata = instance_info["metadata"].toObject();
    QJsonObject device_list = instance_info_metadata["devices"].toObject();

    const QJsonObject new_device_object{{"path", target.c_str()}, {"source", source.c_str()}, {"type", "disk"}};

    device_list.insert(device_name_.c_str(), new_device_object);
    instance_info_metadata["devices"] = device_list;

    const QJsonObject json_reply = lxd_request(network_manager_, "PUT", lxd_instance_endpoint, instance_info_metadata);
    lxd_wait(network_manager_, multipass::lxd_socket_url, json_reply, timeout_millisecond);
}

} // namespace multipass
