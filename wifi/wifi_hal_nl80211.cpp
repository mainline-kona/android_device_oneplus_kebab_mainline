/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure standard Linux nl80211 libwifi-hal implementation for mainline mac80211/cfg80211
 * drivers (such as ath11k_pci on OnePlus 8T / QCA6390).
 */

#define LOG_TAG "WifiHalNl80211"

#include "hardware_legacy/wifi_hal.h"

#include <android-base/logging.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kPrimaryIface = "wlan0";
constexpr const char* kDriverVersion = "ath11k_pci-mainline";
constexpr const char* kFirmwareVersion = "QCA6390-hw2.0";

struct Nl80211HalState;

struct Nl80211Iface {
    Nl80211HalState* hal = nullptr;
    std::string name;
    unsigned int ifindex = 0;
};

struct ChannelInfo {
    int freq = 0;
    bool is_dfs = false;
    bool is_no_ir = false;
    bool no_20mhz = false;
    bool no_ht40 = false;
    bool no_80mhz = false;
    bool no_160mhz = false;
};

struct Nl80211HalState {
    std::mutex mutex;
    struct nl_sock* cmd_sock = nullptr;
    struct nl_sock* event_sock = nullptr;
    int nl80211_id = -1;
    int wiphy_idx = 0;
    int stop_fd = -1;

    std::atomic<bool> stopping{false};
    bool event_loop_running = false;
    std::mutex loop_mutex;
    std::condition_variable loop_cv;

    std::unordered_map<std::string, std::unique_ptr<Nl80211Iface>> ifaces_by_name;
    std::vector<wifi_interface_handle> iface_handle_ptrs;

    wifi_event_handler event_handler{};
    bool has_event_handler = false;

    bool init();
    void teardown();
    void runEventLoop();
    bool sendAndRecvLocked(struct nl_msg* msg, int (*valid_cb)(struct nl_msg*, void*), void* arg,
                           int* out_err = nullptr);
    int resolveWiphyIndexLocked();
    bool refreshIfacesLocked();
    bool queryWiphyChannelsLocked(std::vector<ChannelInfo>* out_channels);
    bool createVirtualIfaceLocked(const char* ifname, enum nl80211_iftype iftype);
    bool deleteVirtualIfaceLocked(const char* ifname);
    bool setCountryCodeLocked(const char alpha2[2]);
};

struct NlWaitCtx {
    int err = 1;
    int (*valid_cb)(struct nl_msg*, void*) = nullptr;
    void* arg = nullptr;
};

int nlErrorCb(struct sockaddr_nl*, struct nlmsgerr* err, void* arg) {
    auto* ctx = static_cast<NlWaitCtx*>(arg);
    ctx->err = err ? err->error : -EIO;
    return NL_STOP;
}

int nlFinishCb(struct nl_msg*, void* arg) {
    auto* ctx = static_cast<NlWaitCtx*>(arg);
    ctx->err = 0;
    return NL_SKIP;
}

int nlAckCb(struct nl_msg*, void* arg) {
    auto* ctx = static_cast<NlWaitCtx*>(arg);
    ctx->err = 0;
    return NL_STOP;
}

int nlValidDispatchCb(struct nl_msg* msg, void* arg) {
    auto* ctx = static_cast<NlWaitCtx*>(arg);
    if (ctx->valid_cb) {
        return ctx->valid_cb(msg, ctx->arg);
    }
    return NL_OK;
}

int resolveMulticastGroup(struct nl_sock* sock, const char* family, const char* group) {
    int ctrl_id = genl_ctrl_resolve(sock, "nlctrl");
    if (ctrl_id < 0) return -1;

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return -1;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, ctrl_id, 0, 0, CTRL_CMD_GETFAMILY, 0);
    nla_put_string(msg, CTRL_ATTR_FAMILY_NAME, family);

    struct GroupCtx {
        const char* group;
        int id;
    } gctx{group, -1};

    NlWaitCtx wait_ctx{};
    wait_ctx.arg = &gctx;
    wait_ctx.valid_cb = [](struct nl_msg* m, void* a) -> int {
        auto* ctx = static_cast<GroupCtx*>(a);
        struct genlmsghdr* gnlh = static_cast<struct genlmsghdr*>(nlmsg_data(nlmsg_hdr(m)));
        struct nlattr* tb[CTRL_ATTR_MAX + 1] = {};
        nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);
        if (!tb[CTRL_ATTR_MCAST_GROUPS]) return NL_SKIP;
        struct nlattr* mcgrp = nullptr;
        int rem = 0;
        nla_for_each_nested(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], rem) {
            struct nlattr* tb_mcgrp[CTRL_ATTR_MCAST_GRP_MAX + 1] = {};
            nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX, static_cast<struct nlattr*>(nla_data(mcgrp)),
                      nla_len(mcgrp), nullptr);
            if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] || !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]) continue;
            if (strncmp(static_cast<const char*>(nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME])),
                        ctx->group, nla_len(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME])) == 0) {
                ctx->id = nla_get_u32(tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]);
                break;
            }
        }
        return NL_SKIP;
    };

    struct nl_cb* cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        nlmsg_free(msg);
        return -1;
    }
    nl_cb_err(cb, NL_CB_CUSTOM, nlErrorCb, &wait_ctx);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nlFinishCb, &wait_ctx);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, nlAckCb, &wait_ctx);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, nlValidDispatchCb, &wait_ctx);

    if (nl_send_auto(sock, msg) >= 0) {
        while (wait_ctx.err > 0) {
            if (nl_recvmsgs(sock, cb) < 0) break;
        }
    }
    nl_cb_put(cb);
    nlmsg_free(msg);
    return gctx.id;
}

int regEventCb(struct nl_msg* msg, void* arg) {
    auto* state = static_cast<Nl80211HalState*>(arg);
    struct genlmsghdr* gnlh = static_cast<struct genlmsghdr*>(nlmsg_data(nlmsg_hdr(msg)));
    if (gnlh->cmd != NL80211_CMD_REG_CHANGE && gnlh->cmd != NL80211_CMD_WIPHY_REG_CHANGE) {
        return NL_SKIP;
    }
    struct nlattr* tb[NL80211_ATTR_MAX + 1] = {};
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);
    if (!tb[NL80211_ATTR_REG_ALPHA2]) return NL_SKIP;
    const char* alpha2 = nla_get_string(tb[NL80211_ATTR_REG_ALPHA2]);
    if (!alpha2 || strlen(alpha2) < 2) return NL_SKIP;

    wifi_event_handler eh{};
    bool has_eh = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        eh = state->event_handler;
        has_eh = state->has_event_handler;
    }
    LOG(INFO) << "nl80211 regulatory domain changed to " << alpha2[0] << alpha2[1];
    if (has_eh && eh.on_country_code_changed) {
        char code[2] = {alpha2[0], alpha2[1]};
        eh.on_country_code_changed(code);
    }
    return NL_SKIP;
}

int seqCheckDisabledCb(struct nl_msg*, void*) {
    return NL_OK;
}

bool setIfaceDown(const char* ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct ifreq ifr{};
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
    bool ok = false;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags &= ~IFF_UP;
        ok = (ioctl(fd, SIOCSIFFLAGS, &ifr) == 0);
    }
    close(fd);
    return ok;
}

bool Nl80211HalState::sendAndRecvLocked(struct nl_msg* msg, int (*valid_cb)(struct nl_msg*, void*),
                                        void* arg, int* out_err) {
    if (!cmd_sock || !msg) return false;

    struct nl_cb* cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) return false;

    NlWaitCtx ctx{};
    ctx.valid_cb = valid_cb;
    ctx.arg = arg;

    nl_cb_err(cb, NL_CB_CUSTOM, nlErrorCb, &ctx);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nlFinishCb, &ctx);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, nlAckCb, &ctx);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, nlValidDispatchCb, &ctx);

    int ret = nl_send_auto(cmd_sock, msg);
    if (ret < 0) {
        nl_cb_put(cb);
        if (out_err) *out_err = ret;
        return false;
    }
    while (ctx.err > 0) {
        int r = nl_recvmsgs(cmd_sock, cb);
        if (r < 0 && ctx.err > 0) {
            ctx.err = r;
            break;
        }
    }
    nl_cb_put(cb);
    if (out_err) *out_err = ctx.err;
    return ctx.err == 0;
}

int Nl80211HalState::resolveWiphyIndexLocked() {
    unsigned int ifidx = if_nametoindex(kPrimaryIface);
    if (ifidx == 0) return 0;

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return 0;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, 0, NL80211_CMD_GET_INTERFACE, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifidx);

    int wiphy = 0;
    sendAndRecvLocked(
            msg,
            [](struct nl_msg* m, void* arg) -> int {
                auto* out_wiphy = static_cast<int*>(arg);
                struct genlmsghdr* gnlh = static_cast<struct genlmsghdr*>(nlmsg_data(nlmsg_hdr(m)));
                struct nlattr* tb[NL80211_ATTR_MAX + 1] = {};
                nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
                          genlmsg_attrlen(gnlh, 0), nullptr);
                if (tb[NL80211_ATTR_WIPHY]) {
                    *out_wiphy = static_cast<int>(nla_get_u32(tb[NL80211_ATTR_WIPHY]));
                }
                return NL_SKIP;
            },
            &wiphy);
    nlmsg_free(msg);
    return wiphy;
}

bool Nl80211HalState::refreshIfacesLocked() {
    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return false;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, NLM_F_DUMP,
                NL80211_CMD_GET_INTERFACE, 0);

    struct DumpCtx {
        int wiphy_idx;
        std::vector<std::pair<std::string, unsigned int>> found;
    } dctx{wiphy_idx, {}};

    bool ok = sendAndRecvLocked(
            msg,
            [](struct nl_msg* m, void* arg) -> int {
                auto* ctx = static_cast<DumpCtx*>(arg);
                struct genlmsghdr* gnlh = static_cast<struct genlmsghdr*>(nlmsg_data(nlmsg_hdr(m)));
                struct nlattr* tb[NL80211_ATTR_MAX + 1] = {};
                nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
                          genlmsg_attrlen(gnlh, 0), nullptr);
                if (tb[NL80211_ATTR_WIPHY] &&
                    static_cast<int>(nla_get_u32(tb[NL80211_ATTR_WIPHY])) != ctx->wiphy_idx) {
                    return NL_SKIP;
                }
                if (!tb[NL80211_ATTR_IFNAME] || !tb[NL80211_ATTR_IFINDEX]) return NL_SKIP;
                const char* name = nla_get_string(tb[NL80211_ATTR_IFNAME]);
                unsigned int idx = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);
                if (name && name[0] != '\0' && idx != 0) {
                    ctx->found.emplace_back(name, idx);
                }
                return NL_SKIP;
            },
            &dctx);
    nlmsg_free(msg);

    if (!ok && dctx.found.empty()) {
        unsigned int idx = if_nametoindex(kPrimaryIface);
        if (idx != 0) dctx.found.emplace_back(kPrimaryIface, idx);
    }

    iface_handle_ptrs.clear();
    for (const auto& [name, idx] : dctx.found) {
        auto it = ifaces_by_name.find(name);
        if (it == ifaces_by_name.end()) {
            auto iface = std::make_unique<Nl80211Iface>();
            iface->hal = this;
            iface->name = name;
            iface->ifindex = idx;
            it = ifaces_by_name.emplace(name, std::move(iface)).first;
        } else {
            it->second->ifindex = idx;
        }
        iface_handle_ptrs.push_back(reinterpret_cast<wifi_interface_handle>(it->second.get()));
    }
    return !iface_handle_ptrs.empty();
}

bool Nl80211HalState::queryWiphyChannelsLocked(std::vector<ChannelInfo>* out_channels) {
    out_channels->clear();

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return false;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, NLM_F_DUMP, NL80211_CMD_GET_WIPHY,
                0);
    nla_put_u32(msg, NL80211_ATTR_WIPHY, static_cast<uint32_t>(wiphy_idx));
    nla_put_flag(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP);

    struct WiphyCtx {
        int wiphy_idx;
        std::map<int, ChannelInfo> by_freq;
    } wctx{wiphy_idx, {}};

    bool ok = sendAndRecvLocked(
            msg,
            [](struct nl_msg* m, void* arg) -> int {
                auto* ctx = static_cast<WiphyCtx*>(arg);
                struct genlmsghdr* gnlh = static_cast<struct genlmsghdr*>(nlmsg_data(nlmsg_hdr(m)));
                struct nlattr* tb[NL80211_ATTR_MAX + 1] = {};
                nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
                          genlmsg_attrlen(gnlh, 0), nullptr);
                if (tb[NL80211_ATTR_WIPHY] &&
                    static_cast<int>(nla_get_u32(tb[NL80211_ATTR_WIPHY])) != ctx->wiphy_idx) {
                    return NL_SKIP;
                }
                if (!tb[NL80211_ATTR_WIPHY_BANDS]) return NL_SKIP;

                struct nlattr* nl_band = nullptr;
                int rem_band = 0;
                nla_for_each_nested(nl_band, tb[NL80211_ATTR_WIPHY_BANDS], rem_band) {
                    struct nlattr* tb_band[NL80211_BAND_ATTR_MAX + 1] = {};
                    nla_parse(tb_band, NL80211_BAND_ATTR_MAX,
                              static_cast<struct nlattr*>(nla_data(nl_band)), nla_len(nl_band),
                              nullptr);
                    if (!tb_band[NL80211_BAND_ATTR_FREQS]) continue;

                    struct nlattr* nl_freq = nullptr;
                    int rem_freq = 0;
                    nla_for_each_nested(nl_freq, tb_band[NL80211_BAND_ATTR_FREQS], rem_freq) {
                        struct nlattr* tb_freq[NL80211_FREQUENCY_ATTR_MAX + 1] = {};
                        nla_parse(tb_freq, NL80211_FREQUENCY_ATTR_MAX,
                                  static_cast<struct nlattr*>(nla_data(nl_freq)), nla_len(nl_freq),
                                  nullptr);
                        if (!tb_freq[NL80211_FREQUENCY_ATTR_FREQ]) continue;
                        if (tb_freq[NL80211_FREQUENCY_ATTR_DISABLED]) continue;

                        ChannelInfo ci{};
                        ci.freq = static_cast<int>(nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_FREQ]));
                        ci.is_dfs = (tb_freq[NL80211_FREQUENCY_ATTR_RADAR] != nullptr);
                        ci.is_no_ir = (tb_freq[NL80211_FREQUENCY_ATTR_NO_IR] != nullptr);
                        ci.no_20mhz = (tb_freq[NL80211_FREQUENCY_ATTR_NO_20MHZ] != nullptr);
                        ci.no_ht40 = (tb_freq[NL80211_FREQUENCY_ATTR_NO_HT40_MINUS] != nullptr &&
                                      tb_freq[NL80211_FREQUENCY_ATTR_NO_HT40_PLUS] != nullptr);
                        ci.no_80mhz = (tb_freq[NL80211_FREQUENCY_ATTR_NO_80MHZ] != nullptr);
                        ci.no_160mhz = (tb_freq[NL80211_FREQUENCY_ATTR_NO_160MHZ] != nullptr);
                        ctx->by_freq[ci.freq] = ci;
                    }
                }
                return NL_SKIP;
            },
            &wctx);
    nlmsg_free(msg);

    for (const auto& [_, ci] : wctx.by_freq) {
        out_channels->push_back(ci);
    }
    return ok && !out_channels->empty();
}

bool Nl80211HalState::createVirtualIfaceLocked(const char* ifname, enum nl80211_iftype iftype) {
    if (!ifname || ifname[0] == '\0') return false;
    if (if_nametoindex(ifname) != 0) {
        LOG(INFO) << "Virtual interface " << ifname << " already exists";
        refreshIfacesLocked();
        return true;
    }

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return false;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, 0, NL80211_CMD_NEW_INTERFACE, 0);
    nla_put_u32(msg, NL80211_ATTR_WIPHY, static_cast<uint32_t>(wiphy_idx));
    nla_put_string(msg, NL80211_ATTR_IFNAME, ifname);
    nla_put_u32(msg, NL80211_ATTR_IFTYPE, static_cast<uint32_t>(iftype));

    int err = 0;
    bool ok = sendAndRecvLocked(msg, nullptr, nullptr, &err);
    nlmsg_free(msg);

    if (!ok && err != -EEXIST) {
        LOG(ERROR) << "NL80211_CMD_NEW_INTERFACE(" << ifname << ", type=" << iftype
                   << ") failed: " << err;
        return false;
    }
    LOG(INFO) << "Created virtual nl80211 interface " << ifname << " (type=" << iftype << ")";
    refreshIfacesLocked();
    return true;
}

bool Nl80211HalState::deleteVirtualIfaceLocked(const char* ifname) {
    if (!ifname || ifname[0] == '\0') return false;
    if (strcmp(ifname, kPrimaryIface) == 0) {
        LOG(INFO) << "Ignoring delete request for primary interface " << kPrimaryIface;
        return true;
    }
    unsigned int ifidx = if_nametoindex(ifname);
    if (ifidx == 0) {
        refreshIfacesLocked();
        return true;
    }

    setIfaceDown(ifname);

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return false;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, 0, NL80211_CMD_DEL_INTERFACE, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifidx);

    int err = 0;
    bool ok = sendAndRecvLocked(msg, nullptr, nullptr, &err);
    nlmsg_free(msg);

    if (!ok && err != -ENODEV) {
        LOG(ERROR) << "NL80211_CMD_DEL_INTERFACE(" << ifname << ") failed: " << err;
        return false;
    }
    LOG(INFO) << "Deleted virtual nl80211 interface " << ifname;
    refreshIfacesLocked();
    return true;
}

bool Nl80211HalState::setCountryCodeLocked(const char alpha2[2]) {
    char cc[3] = {static_cast<char>(std::toupper(static_cast<unsigned char>(alpha2[0]))),
                  static_cast<char>(std::toupper(static_cast<unsigned char>(alpha2[1]))), '\0'};

    struct nl_msg* msg = nlmsg_alloc();
    if (!msg) return false;
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, 0, NL80211_CMD_REQ_SET_REG, 0);
    nla_put_string(msg, NL80211_ATTR_REG_ALPHA2, cc);

    int err = 0;
    bool ok = sendAndRecvLocked(msg, nullptr, nullptr, &err);
    nlmsg_free(msg);

    if (!ok && err != -EALREADY) {
        LOG(ERROR) << "NL80211_CMD_REQ_SET_REG(" << cc << ") failed: " << err;
        return false;
    }
    LOG(INFO) << "Requested nl80211 country code " << cc;
    return true;
}

bool Nl80211HalState::init() {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = false;

    cmd_sock = nl_socket_alloc();
    if (!cmd_sock || genl_connect(cmd_sock) < 0) {
        LOG(ERROR) << "Failed to open nl80211 command netlink socket";
        return false;
    }
    nl_socket_set_buffer_size(cmd_sock, 65536, 65536);

    nl80211_id = genl_ctrl_resolve(cmd_sock, "nl80211");
    if (nl80211_id < 0) {
        LOG(ERROR) << "Failed to resolve nl80211 genl family";
        return false;
    }

    stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd < 0) {
        LOG(ERROR) << "Failed to create stop eventfd";
        return false;
    }

    // Optional regulatory event listener socket
    event_sock = nl_socket_alloc();
    if (event_sock && genl_connect(event_sock) == 0) {
        int reg_mcgrp = resolveMulticastGroup(event_sock, "nl80211", "regulatory");
        if (reg_mcgrp >= 0 && nl_socket_add_membership(event_sock, reg_mcgrp) == 0) {
            nl_socket_set_nonblocking(event_sock);
            nl_socket_modify_cb(event_sock, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, seqCheckDisabledCb,
                                nullptr);
            nl_socket_modify_cb(event_sock, NL_CB_VALID, NL_CB_CUSTOM, regEventCb, this);
        } else {
            nl_socket_free(event_sock);
            event_sock = nullptr;
        }
    }

    wiphy_idx = resolveWiphyIndexLocked();
    refreshIfacesLocked();
    LOG(INFO) << "Initialized pure-nl80211 Wi-Fi HAL (wiphy=" << wiphy_idx
              << ", ifaces=" << iface_handle_ptrs.size() << ")";
    return !iface_handle_ptrs.empty();
}

void Nl80211HalState::runEventLoop() {
    {
        std::lock_guard<std::mutex> lock(loop_mutex);
        event_loop_running = true;
    }

    while (!stopping.load()) {
        struct pollfd pfds[2] = {};
        nfds_t nfds = 0;
        pfds[nfds].fd = stop_fd;
        pfds[nfds].events = POLLIN;
        nfds++;

        if (event_sock) {
            pfds[nfds].fd = nl_socket_get_fd(event_sock);
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(pfds, nfds, 500);
        if (ret <= 0) continue;

        if (pfds[0].revents & POLLIN) {
            uint64_t val = 0;
            (void)read(stop_fd, &val, sizeof(val));
            break;
        }
        if (nfds > 1 && (pfds[1].revents & POLLIN) && event_sock) {
            nl_recvmsgs_default(event_sock);
        }
    }

    {
        std::lock_guard<std::mutex> lock(loop_mutex);
        event_loop_running = false;
    }
    loop_cv.notify_all();
}

void Nl80211HalState::teardown() {
    stopping = true;
    if (stop_fd >= 0) {
        uint64_t one = 1;
        (void)write(stop_fd, &one, sizeof(one));
    }

    {
        std::unique_lock<std::mutex> lock(loop_mutex);
        loop_cv.wait_for(lock, std::chrono::seconds(2),
                         [this] { return !event_loop_running; });
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (event_sock) {
        nl_socket_free(event_sock);
        event_sock = nullptr;
    }
    if (cmd_sock) {
        nl_socket_free(cmd_sock);
        cmd_sock = nullptr;
    }
    if (stop_fd >= 0) {
        close(stop_fd);
        stop_fd = -1;
    }
    iface_handle_ptrs.clear();
    ifaces_by_name.clear();
    has_event_handler = false;
}

Nl80211HalState* asHal(wifi_handle h) {
    return reinterpret_cast<Nl80211HalState*>(h);
}

Nl80211Iface* asIface(wifi_interface_handle h) {
    return reinterpret_cast<Nl80211Iface*>(h);
}

constexpr feature_set kSupportedFeatures =
        WIFI_FEATURE_INFRA | WIFI_FEATURE_INFRA_5G | WIFI_FEATURE_P2P | WIFI_FEATURE_SOFT_AP |
        WIFI_FEATURE_AP_STA | WIFI_FEATURE_ADDITIONAL_STA | WIFI_FEATURE_TDLS;

wifi_error nl_wifi_wait_for_driver_ready(void) {
    for (int i = 0; i < 100; ++i) {
        if (if_nametoindex(kPrimaryIface) != 0) {
            return WIFI_SUCCESS;
        }
        usleep(100 * 1000);
    }
    LOG(ERROR) << "Timed out waiting for " << kPrimaryIface;
    return WIFI_ERROR_TIMED_OUT;
}

wifi_error nl_wifi_initialize(wifi_handle* handle) {
    if (!handle) return WIFI_ERROR_INVALID_ARGS;
    auto* state = new Nl80211HalState();
    if (!state->init()) {
        state->teardown();
        delete state;
        return WIFI_ERROR_UNKNOWN;
    }
    *handle = reinterpret_cast<wifi_handle>(state);
    return WIFI_SUCCESS;
}

void nl_wifi_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler) {
    if (!handle) return;
    auto* state = asHal(handle);
    state->teardown();
    if (handler) {
        handler(handle);
    }
    delete state;
}

void nl_wifi_event_loop(wifi_handle handle) {
    if (!handle) return;
    asHal(handle)->runEventLoop();
}

void nl_wifi_get_error_info(wifi_error err, const char** msg) {
    if (!msg) return;
    switch (err) {
        case WIFI_SUCCESS:
            *msg = "WIFI_SUCCESS";
            break;
        case WIFI_ERROR_NOT_SUPPORTED:
            *msg = "WIFI_ERROR_NOT_SUPPORTED";
            break;
        case WIFI_ERROR_NOT_AVAILABLE:
            *msg = "WIFI_ERROR_NOT_AVAILABLE";
            break;
        case WIFI_ERROR_INVALID_ARGS:
            *msg = "WIFI_ERROR_INVALID_ARGS";
            break;
        case WIFI_ERROR_TIMED_OUT:
            *msg = "WIFI_ERROR_TIMED_OUT";
            break;
        default:
            *msg = "WIFI_ERROR_UNKNOWN";
            break;
    }
}

wifi_error nl_wifi_get_ifaces(wifi_handle handle, int* num, wifi_interface_handle** interfaces) {
    if (!handle || !num || !interfaces) return WIFI_ERROR_INVALID_ARGS;
    auto* state = asHal(handle);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->refreshIfacesLocked();
    *num = static_cast<int>(state->iface_handle_ptrs.size());
    *interfaces = state->iface_handle_ptrs.empty() ? nullptr : state->iface_handle_ptrs.data();
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_iface_name(wifi_interface_handle handle, char* name, size_t size) {
    if (!handle || !name || size == 0) return WIFI_ERROR_INVALID_ARGS;
    auto* iface = asIface(handle);
    if (size < iface->name.size() + 1) return WIFI_ERROR_INVALID_ARGS;
    strlcpy(name, iface->name.c_str(), size);
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_supported_feature_set(wifi_interface_handle handle, feature_set* set) {
    if (!handle || !set) return WIFI_ERROR_INVALID_ARGS;
    *set = kSupportedFeatures;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_chip_feature_set(wifi_handle handle, feature_set* set) {
    if (!handle || !set) return WIFI_ERROR_INVALID_ARGS;
    *set = kSupportedFeatures;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_virtual_interface_create(wifi_handle handle, const char* ifname,
                                            wifi_interface_type iface_type) {
    if (!handle || !ifname) return WIFI_ERROR_INVALID_ARGS;
    enum nl80211_iftype nl_type = NL80211_IFTYPE_STATION;
    switch (iface_type) {
        case WIFI_INTERFACE_TYPE_AP:
        case WIFI_INTERFACE_TYPE_AP_BRIDGED:
            nl_type = NL80211_IFTYPE_AP;
            break;
        case WIFI_INTERFACE_TYPE_P2P:
            nl_type = NL80211_IFTYPE_P2P_CLIENT;
            break;
        case WIFI_INTERFACE_TYPE_STA:
        default:
            nl_type = NL80211_IFTYPE_STATION;
            break;
    }
    auto* state = asHal(handle);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->createVirtualIfaceLocked(ifname, nl_type) ? WIFI_SUCCESS : WIFI_ERROR_UNKNOWN;
}

wifi_error nl_wifi_virtual_interface_create_with_vendor_data(wifi_handle handle, const char* ifname,
                                                             wifi_interface_type iface_type,
                                                             wifi_vendor_data* /*vendor_data*/) {
    return nl_wifi_virtual_interface_create(handle, ifname, iface_type);
}

wifi_error nl_wifi_virtual_interface_delete(wifi_handle handle, const char* ifname) {
    if (!handle || !ifname) return WIFI_ERROR_INVALID_ARGS;
    auto* state = asHal(handle);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->deleteVirtualIfaceLocked(ifname) ? WIFI_SUCCESS : WIFI_ERROR_UNKNOWN;
}

wifi_error nl_wifi_set_country_code(wifi_interface_handle handle, const char* country_code) {
    if (!handle || !country_code || strlen(country_code) < 2) return WIFI_ERROR_INVALID_ARGS;
    if (!std::isalpha(static_cast<unsigned char>(country_code[0])) ||
        !std::isalpha(static_cast<unsigned char>(country_code[1]))) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    auto* state = asIface(handle)->hal;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->setCountryCodeLocked(country_code) ? WIFI_SUCCESS : WIFI_ERROR_UNKNOWN;
}

wifi_error nl_wifi_get_valid_channels(wifi_interface_handle handle, int band, int max_channels,
                                      wifi_channel* channels, int* num_channels) {
    if (!handle || !channels || !num_channels || max_channels <= 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    auto* state = asIface(handle)->hal;
    std::vector<ChannelInfo> all;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->queryWiphyChannelsLocked(&all)) {
            *num_channels = 0;
            return WIFI_ERROR_UNKNOWN;
        }
    }

    int count = 0;
    for (const auto& ci : all) {
        bool is_2g = (ci.freq >= 2400 && ci.freq < 2500);
        bool is_5g = (ci.freq >= 4900 && ci.freq < 5900);
        bool match = false;
        switch (band) {
            case WIFI_BAND_BG:
                match = is_2g;
                break;
            case WIFI_BAND_A:
                match = is_5g && !ci.is_dfs;
                break;
            case WIFI_BAND_A_DFS:
                match = is_5g && ci.is_dfs;
                break;
            case WIFI_BAND_A_WITH_DFS:
                match = is_5g;
                break;
            case WIFI_BAND_ABG:
                match = is_2g || (is_5g && !ci.is_dfs);
                break;
            case WIFI_BAND_ABG_WITH_DFS:
                match = is_2g || is_5g;
                break;
            default:
                match = false;
                break;
        }
        if (match && count < max_channels) {
            channels[count++] = ci.freq;
        }
    }
    *num_channels = count;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_usable_channels(wifi_handle handle, u32 band_mask, u32 iface_mode_mask,
                                       u32 /*filter_mask*/, u32 max_size, u32* size,
                                       wifi_usable_channel* channels) {
    if (!handle || !size || !channels || max_size == 0) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    auto* state = asHal(handle);
    std::vector<ChannelInfo> all;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->queryWiphyChannelsLocked(&all)) {
            *size = 0;
            return WIFI_ERROR_UNKNOWN;
        }
    }

    u32 count = 0;
    for (const auto& ci : all) {
        u32 ch_band = 0;
        if (ci.freq >= 2400 && ci.freq < 2500) {
            ch_band = WLAN_MAC_2_4_BAND;
        } else if (ci.freq >= 4900 && ci.freq < 5900) {
            ch_band = WLAN_MAC_5_0_BAND;
        } else if (ci.freq >= 5925 && ci.freq <= 7125) {
            ch_band = WLAN_MAC_6_0_BAND;
        } else {
            continue;
        }
        if ((band_mask & ch_band) == 0) continue;

        u32 allowed_modes = (1U << WIFI_INTERFACE_STA);
        if (!ci.is_no_ir && !ci.is_dfs) {
            allowed_modes |= (1U << WIFI_INTERFACE_SOFTAP) | (1U << WIFI_INTERFACE_P2P_GO) |
                             (1U << WIFI_INTERFACE_P2P_CLIENT) | (1U << WIFI_INTERFACE_TDLS);
        }
        u32 effective_modes = allowed_modes & iface_mode_mask;
        if (effective_modes == 0) continue;

        wifi_channel_width width = WIFI_CHAN_WIDTH_20;
        if (ch_band == WLAN_MAC_5_0_BAND || ch_band == WLAN_MAC_6_0_BAND) {
            if (!ci.no_80mhz) {
                width = WIFI_CHAN_WIDTH_80;
            } else if (!ci.no_ht40) {
                width = WIFI_CHAN_WIDTH_40;
            }
        } else if (!ci.no_ht40) {
            width = WIFI_CHAN_WIDTH_40;
        }

        if (count < max_size) {
            channels[count].freq = ci.freq;
            channels[count].width = width;
            channels[count].iface_mode_mask = effective_modes;
            count++;
        }
    }
    *size = count;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_set_iface_event_handler(wifi_request_id /*id*/, wifi_interface_handle iface,
                                           wifi_event_handler eh) {
    if (!iface) return WIFI_ERROR_INVALID_ARGS;
    auto* state = asIface(iface)->hal;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->event_handler = eh;
    state->has_event_handler = true;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_reset_iface_event_handler(wifi_request_id /*id*/, wifi_interface_handle iface) {
    if (!iface) return WIFI_ERROR_INVALID_ARGS;
    auto* state = asIface(iface)->hal;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->event_handler = {};
    state->has_event_handler = false;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_driver_version(wifi_interface_handle handle, char* buffer, int buffer_size) {
    if (!handle || !buffer || buffer_size <= 0) return WIFI_ERROR_INVALID_ARGS;
    strlcpy(buffer, kDriverVersion, static_cast<size_t>(buffer_size));
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_firmware_version(wifi_interface_handle handle, char* buffer,
                                        int buffer_size) {
    if (!handle || !buffer || buffer_size <= 0) return WIFI_ERROR_INVALID_ARGS;
    strlcpy(buffer, kFirmwareVersion, static_cast<size_t>(buffer_size));
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_get_logger_supported_feature_set(wifi_interface_handle handle,
                                                    unsigned int* support) {
    if (!handle || !support) return WIFI_ERROR_INVALID_ARGS;
    *support = 0;
    return WIFI_SUCCESS;
}

wifi_error nl_wifi_set_nodfs_flag(wifi_interface_handle handle, u32 /*nodfs*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_set_scanning_mac_oui(wifi_interface_handle handle, unsigned char* /*oui*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_configure_nd_offload(wifi_interface_handle handle, u8 /*enable*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_set_dtim_config(wifi_interface_handle handle, u32 /*multiplier*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_set_scan_mode(wifi_interface_handle handle, bool /*enable*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_select_tx_power_scenario(wifi_interface_handle handle,
                                            wifi_power_scenario /*scenario*/) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_reset_tx_power_scenario(wifi_interface_handle handle) {
    return handle ? WIFI_SUCCESS : WIFI_ERROR_INVALID_ARGS;
}

wifi_error nl_wifi_early_initialize(void) {
    return WIFI_SUCCESS;
}

}  // namespace

extern "C" wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn) {
    if (!fn) return WIFI_ERROR_INVALID_ARGS;

    fn->wifi_early_initialize = nl_wifi_early_initialize;
    fn->wifi_wait_for_driver_ready = nl_wifi_wait_for_driver_ready;
    fn->wifi_initialize = nl_wifi_initialize;
    fn->wifi_cleanup = nl_wifi_cleanup;
    fn->wifi_event_loop = nl_wifi_event_loop;
    fn->wifi_get_error_info = nl_wifi_get_error_info;

    fn->wifi_get_ifaces = nl_wifi_get_ifaces;
    fn->wifi_get_iface_name = nl_wifi_get_iface_name;
    fn->wifi_get_supported_feature_set = nl_wifi_get_supported_feature_set;
    fn->wifi_get_chip_feature_set = nl_wifi_get_chip_feature_set;

    fn->wifi_virtual_interface_create = nl_wifi_virtual_interface_create;
    fn->wifi_virtual_interface_create_with_vendor_data =
            nl_wifi_virtual_interface_create_with_vendor_data;
    fn->wifi_virtual_interface_delete = nl_wifi_virtual_interface_delete;

    fn->wifi_set_country_code = nl_wifi_set_country_code;
    fn->wifi_get_valid_channels = nl_wifi_get_valid_channels;
    fn->wifi_get_usable_channels = nl_wifi_get_usable_channels;

    fn->wifi_set_iface_event_handler = nl_wifi_set_iface_event_handler;
    fn->wifi_reset_iface_event_handler = nl_wifi_reset_iface_event_handler;
    fn->wifi_get_driver_version = nl_wifi_get_driver_version;
    fn->wifi_get_firmware_version = nl_wifi_get_firmware_version;
    fn->wifi_get_logger_supported_feature_set = nl_wifi_get_logger_supported_feature_set;

    fn->wifi_set_nodfs_flag = nl_wifi_set_nodfs_flag;
    fn->wifi_set_scanning_mac_oui = nl_wifi_set_scanning_mac_oui;
    fn->wifi_configure_nd_offload = nl_wifi_configure_nd_offload;
    fn->wifi_set_dtim_config = nl_wifi_set_dtim_config;
    fn->wifi_set_scan_mode = nl_wifi_set_scan_mode;
    fn->wifi_select_tx_power_scenario = nl_wifi_select_tx_power_scenario;
    fn->wifi_reset_tx_power_scenario = nl_wifi_reset_tx_power_scenario;

    return WIFI_SUCCESS;
}
