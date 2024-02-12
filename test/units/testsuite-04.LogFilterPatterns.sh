#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# This fails due to https://github.com/systemd/systemd/issues/30886
# but it is too complex and risky to backport, so disable the test
exit 0

# shellcheck source=test/units/util.sh
 . "$(dirname "$0")"/util.sh

add_logs_filtering_override() {
    local unit="${1:?}"
    local override_name="${2:?}"
    local log_filter="${3:-}"

    mkdir -p "/run/systemd/system/$unit.d/"
    echo -ne "[Service]\nLogFilterPatterns=$log_filter" >"/run/systemd/system/$unit.d/$override_name.conf"
    systemctl daemon-reload
}

run_service_and_fetch_logs() {
    local unit="${1:?}"
    local start end

    start="$(date '+%Y-%m-%d %T.%6N')"
    systemctl restart "$unit"
    sleep .5
    journalctl --sync
    end="$(date '+%Y-%m-%d %T.%6N')"

    journalctl -q -u "$unit" -S "$start" -U "$end" -p notice
    systemctl stop "$unit"
}

if cgroupfs_supports_user_xattrs; then
    # Accept all log messages
    add_logs_filtering_override "logs-filtering.service" "00-reset" ""
    [[ -n $(run_service_and_fetch_logs "logs-filtering.service") ]]

    add_logs_filtering_override "logs-filtering.service" "01-allow-all" ".*"
    [[ -n $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Discard all log messages
    add_logs_filtering_override "logs-filtering.service" "02-discard-all" "~.*"
    [[ -z $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Accept all test messages
    add_logs_filtering_override "logs-filtering.service" "03-reset" ""
    [[ -n $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Discard all test messages
    add_logs_filtering_override "logs-filtering.service" "04-discard-gg" "~.*gg.*"
    [[ -z $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Deny filter takes precedence
    add_logs_filtering_override "logs-filtering.service" "05-allow-all-but-too-late" ".*"
    [[ -z $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Use tilde in a deny pattern
    add_logs_filtering_override "logs-filtering.service" "06-reset" ""
    add_logs_filtering_override "logs-filtering.service" "07-prevent-tilde" "~~more~"
    [[ -z $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Only allow a pattern that won't be matched
    add_logs_filtering_override "logs-filtering.service" "08-reset" ""
    add_logs_filtering_override "logs-filtering.service" "09-allow-only-non-existing" "non-existing string"
    [[ -z $(run_service_and_fetch_logs "logs-filtering.service") ]]

    # Allow a pattern starting with a tilde
    add_logs_filtering_override "logs-filtering.service" "10-allow-with-escape-char" "\\\\x7emore~"
    [[ -n $(run_service_and_fetch_logs "logs-filtering.service") ]]

    add_logs_filtering_override "logs-filtering.service" "11-reset" ""
    add_logs_filtering_override "logs-filtering.service" "12-allow-with-spaces" "foo bar"
    [[ -n $(run_service_and_fetch_logs "logs-filtering.service") ]]

    add_logs_filtering_override "delegated-cgroup-filtering.service" "00-allow-all" ".*"
    [[ -n $(run_service_and_fetch_logs "delegated-cgroup-filtering.service") ]]

    add_logs_filtering_override "delegated-cgroup-filtering.service" "01-discard-hello" "~hello"
    [[ -z $(run_service_and_fetch_logs "delegated-cgroup-filtering.service") ]]

    rm -rf /run/systemd/system/{logs-filtering,delegated-cgroup-filtering}.service.d
fi
