#!/usr/bin/env bash
set -euo pipefail

HOST_IP="192.168.0.10"
PREFIX="24"
FPGA_IP="192.168.0.42"
CONTROL_PORT="4000"
DATA_PORT="5000"
DRY_RUN=0
CHECK_ONLY=0
IFACE="eno1"
YES=0

usage() {
  cat <<USAGE
Usage: $0 [--dry-run] [--check] [--iface IFACE] [--yes]

Configure the host for the NCLP board network:
  - assigns ${HOST_IP}/${PREFIX} to the selected Ethernet interface
  - allows outbound TCP control to ${FPGA_IP}:${CONTROL_PORT}
  - allows inbound UDP samples from ${FPGA_IP} to port ${DATA_PORT}

Options:
  --dry-run      Print commands without running them
  --check        Only report whether the host config already looks correct
  --iface IFACE  Use a specific Ethernet interface (default: eno1)
  --yes          Do not prompt before applying changes
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --check) CHECK_ONLY=1; shift ;;
    --iface) IFACE="${2:-}"; shift 2 ;;
    --yes|-y) YES=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

run() {
  echo "+ $*"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    "$@"
  fi
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

is_default_iface() {
  local iface="$1"
  ip route show default 2>/dev/null | grep -Eq " dev ${iface}( |$)"
}

iface_has_board_subnet() {
  local iface="$1"
  local subnet_prefix="${HOST_IP%.*}."
  ip -4 addr show dev "$iface" 2>/dev/null | grep -Fq "inet ${subnet_prefix}"
}

link_state() {
  local iface="$1"
  cat "/sys/class/net/${iface}/operstate" 2>/dev/null || echo unknown
}

pick_iface() {
  if [[ -n "$IFACE" ]]; then
    echo "$IFACE"
    return
  fi

  local candidates=()
  local already=()
  while IFS= read -r iface; do
    [[ "$iface" == "lo" ]] && continue
    [[ ! -d "/sys/class/net/${iface}" ]] && continue
    if [[ -d "/sys/class/net/${iface}/wireless" ]]; then
      continue
    fi
    if iface_has_board_subnet "$iface"; then
      already+=("$iface")
      continue
    fi
    if [[ "$(link_state "$iface")" == "up" ]] && ! is_default_iface "$iface"; then
      candidates+=("$iface")
    fi
  done < <(ls /sys/class/net)

  if [[ "${#already[@]}" -eq 1 ]]; then
    echo "${already[0]}"
    return
  fi
  if [[ "${#candidates[@]}" -eq 1 ]]; then
    echo "${candidates[0]}"
    return
  fi

  echo "Could not safely auto-detect one Ethernet interface." >&2
  echo "Interfaces:" >&2
  ip -brief link >&2 || true
  echo "" >&2
  echo "Rerun with --iface <name>, for example: sudo $0 --iface eno1" >&2
  exit 1
}

firewall_status() {
  if need_cmd ufw && ufw status 2>/dev/null | grep -qi "Status: active"; then
    local missing=0
    local status
    status=$(ufw status numbered 2>/dev/null)
    printf '%s\n' "$status" | grep -Eq \
      "(${CONTROL_PORT}/tcp.*${FPGA_IP}|${FPGA_IP}.*${CONTROL_PORT}/tcp)" || missing=1
    printf '%s\n' "$status" | grep -Eq \
      "(${DATA_PORT}/udp.*${FPGA_IP}|${FPGA_IP}.*${DATA_PORT}/udp)" || missing=1
    return "$missing"
  fi

  if need_cmd firewall-cmd && firewall-cmd --state >/dev/null 2>&1; then
    local data_rule
    data_rule="rule family=\"ipv4\" source address=\"${FPGA_IP}\" destination address=\"${HOST_IP}\" port port=\"${DATA_PORT}\" protocol=\"udp\" accept"
    # firewalld permits outbound connections and their established replies by
    # default; only the board's inbound sample datagrams need an ingress rule.
    firewall-cmd --query-rich-rule="$data_rule" >/dev/null 2>&1
    return $?
  fi

  if need_cmd iptables; then
    iptables -C OUTPUT -p tcp -d "$FPGA_IP" --dport "$CONTROL_PORT" \
      -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT >/dev/null 2>&1 || return 1
    iptables -C INPUT -p tcp -s "$FPGA_IP" --sport "$CONTROL_PORT" \
      -m conntrack --ctstate ESTABLISHED -j ACCEPT >/dev/null 2>&1 || return 1
    iptables -C INPUT -p udp -s "$FPGA_IP" -d "$HOST_IP" \
      --dport "$DATA_PORT" -j ACCEPT >/dev/null 2>&1 || return 1
    return 0
  fi

  return 2
}

report_status() {
  local iface="$1"
  local ip_ok=1
  local fw_state=2

  if ip -4 addr show dev "$iface" 2>/dev/null | grep -q "${HOST_IP}/${PREFIX}"; then
    ip_ok=0
  fi

  if firewall_status; then
    fw_state=0
  else
    fw_state=$?
  fi

  echo "Interface: ${iface}"
  if [[ "$ip_ok" -eq 0 ]]; then
    echo "IP status: OK (${HOST_IP}/${PREFIX} present)"
  else
    echo "IP status: MISSING (${HOST_IP}/${PREFIX} not present)"
  fi

  case "$fw_state" in
    0) echo "Firewall status: OK" ;;
    1) echo "Firewall status: MISSING one or more rules" ;;
    *) echo "Firewall status: UNKNOWN (no supported firewall tool detected)" ;;
  esac

  if [[ "$ip_ok" -eq 0 && "$fw_state" -eq 0 ]]; then
    return 0
  fi
  return 1
}

configure_ip() {
  local iface="$1"

  if ip -4 addr show dev "$iface" | grep -q "${HOST_IP}/${PREFIX}"; then
    echo "${iface} already has ${HOST_IP}/${PREFIX}"
    return
  fi

  if need_cmd nmcli && nmcli -t -f DEVICE,STATE dev status 2>/dev/null | grep -q "^${iface}:"; then
    local con
    con=$(nmcli -t -f NAME,DEVICE con show --active 2>/dev/null | awk -F: -v dev="$iface" '$2 == dev {print $1; exit}')
    if [[ -n "$con" ]]; then
      run nmcli con mod "$con" ipv4.addresses "${HOST_IP}/${PREFIX}" ipv4.method manual ipv4.gateway "" ipv4.never-default yes
      run nmcli con up "$con"
      return
    fi
  fi

  run ip addr flush dev "$iface" scope global
  run ip addr add "${HOST_IP}/${PREFIX}" dev "$iface"
  run ip link set "$iface" up
}

configure_firewall() {
  echo "Configuring firewall rules for NCLP TCP control and UDP samples."

  if need_cmd ufw && ufw status 2>/dev/null | grep -qi "Status: active"; then
    run ufw allow out to "$FPGA_IP" port "$CONTROL_PORT" proto tcp comment "NCLP TCP control"
    run ufw allow in from "$FPGA_IP" to "$HOST_IP" port "$DATA_PORT" proto udp comment "NCLP UDP samples"
    return
  fi

  if need_cmd firewall-cmd && firewall-cmd --state >/dev/null 2>&1; then
    local data_rule
    data_rule="rule family=\"ipv4\" source address=\"${FPGA_IP}\" destination address=\"${HOST_IP}\" port port=\"${DATA_PORT}\" protocol=\"udp\" accept"
    run firewall-cmd --add-rich-rule="$data_rule"
    run firewall-cmd --permanent --add-rich-rule="$data_rule"
    echo "firewalld permits outbound TCP and established replies by default."
    return
  fi

  if need_cmd iptables; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
      run iptables -A OUTPUT -p tcp -d "$FPGA_IP" --dport "$CONTROL_PORT" -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT
      run iptables -A INPUT -p tcp -s "$FPGA_IP" --sport "$CONTROL_PORT" -m conntrack --ctstate ESTABLISHED -j ACCEPT
      run iptables -A INPUT -p udp -s "$FPGA_IP" -d "$HOST_IP" --dport "$DATA_PORT" -j ACCEPT
    else
      run iptables -C OUTPUT -p tcp -d "$FPGA_IP" --dport "$CONTROL_PORT" -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT 2>/dev/null || \
        run iptables -A OUTPUT -p tcp -d "$FPGA_IP" --dport "$CONTROL_PORT" -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT
      run iptables -C INPUT -p tcp -s "$FPGA_IP" --sport "$CONTROL_PORT" -m conntrack --ctstate ESTABLISHED -j ACCEPT 2>/dev/null || \
        run iptables -A INPUT -p tcp -s "$FPGA_IP" --sport "$CONTROL_PORT" -m conntrack --ctstate ESTABLISHED -j ACCEPT
      run iptables -C INPUT -p udp -s "$FPGA_IP" -d "$HOST_IP" --dport "$DATA_PORT" -j ACCEPT 2>/dev/null || \
        run iptables -A INPUT -p udp -s "$FPGA_IP" -d "$HOST_IP" --dport "$DATA_PORT" -j ACCEPT
    fi
    echo "Note: iptables rules may not persist after reboot unless your distro saves them."
    return
  fi

  cat <<MANUAL
No supported firewall tool detected.
Open these paths manually:
  outbound TCP ${FPGA_IP}:${CONTROL_PORT} and established replies
  inbound UDP from ${FPGA_IP} to ${HOST_IP}:${DATA_PORT}
MANUAL
}

main() {
  local iface
  iface=$(pick_iface)

  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    report_status "$iface"
    exit $?
  fi

  if [[ "$DRY_RUN" -eq 0 && "$EUID" -ne 0 ]]; then
    echo "This script needs root for network/firewall changes. Rerun with sudo, or use --dry-run/--check." >&2
    exit 1
  fi

  echo "Selected interface: ${iface}"
  echo "Will configure: ${HOST_IP}/${PREFIX}"
  echo "Will allow TCP control: outbound ${FPGA_IP}:${CONTROL_PORT}"
  echo "Will allow UDP samples: inbound ${FPGA_IP} -> ${HOST_IP}:${DATA_PORT}"

  if [[ "$DRY_RUN" -eq 0 && "$YES" -eq 0 ]]; then
    read -r -p "Apply these changes? [y/N] " answer
    case "$answer" in
      y|Y|yes|YES) ;;
      *) echo "Aborted."; exit 0 ;;
    esac
  fi

  configure_ip "$iface"
  configure_firewall

  echo "Done. Test production control/data with scripts/test_main_ps_network.py."
}

main "$@"
