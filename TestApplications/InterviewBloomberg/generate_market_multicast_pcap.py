import argparse
from datetime import datetime, timezone
import calendar
import random
from scapy.all import Ether, IP, UDP, Raw, wrpcap

OUT_FILE = "synthetic_market_multicast.pcap"
SRC_MAC = "02:00:00:00:00:01"

MULTICAST_CHANNELS = [
    ("239.10.10.10", 5000),
    ("239.10.10.11", 5001),
    ("239.10.10.12", 5002),
    ("239.20.20.1",  5100),
]

EXCHANGES = [
    "EXBYTE_XNAS",
    "EXBYTE_XNYS",
    "EXBYTE_XCME",
    "EXBYTE_XLON",
]

objects = ["AAPL", "MSFT", "GOOG", "TSLA", "NVDA", "EURUSD", "BTCUSD"]
msg_types = ["TRADE", "QUOTE", "BOOK"]
DEFAULT_SEED = 42


def multicast_ip_to_mac(ip_addr: str) -> str:
    # IPv4 multicast maps to 01:00:5e:xx:xx:xx with lower 23 bits of group.
    a, b, c, d = [int(part) for part in ip_addr.split(".")]
    low23 = ((a & 0x7F) << 16) | (b << 8) | c
    return f"01:00:5e:{(low23 >> 16) & 0x7F:02x}:{(low23 >> 8) & 0xFF:02x}:{d:02x}"


def format_utc_ns(epoch_ns: int) -> str:
    sec, nsec = divmod(epoch_ns, 1_000_000_000)
    dt = datetime.fromtimestamp(sec, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S") + f".{nsec:09d}Z"

parser = argparse.ArgumentParser(
    description="Generate synthetic UDP multicast market-data pcap."
)
parser.add_argument(
    "--random",
    action="store_true",
    help="Use non-deterministic random behavior (default is deterministic).",
)
args = parser.parse_args()

if not args.random:
    random.seed(DEFAULT_SEED)

start = datetime(2026, 4, 13, 12, 0, 0)
packets = []
start_epoch_ns = calendar.timegm(start.utctimetuple()) * 1_000_000_000
current_offset_ns = 0

for i in range(1, 251):
    # Bursty timing: many very small gaps punctuated by occasional larger gaps.
    r = random.random()
    if r < 0.72:
        delta_ns = random.randint(100_000, 900_000)
    elif r < 0.94:
        delta_ns = int(random.uniform(3_000_000, 20_000_000))
    else:
        delta_ns = int(random.uniform(50_000_000, 200_000_000))

    current_offset_ns += delta_ns
    ts = format_utc_ns(start_epoch_ns + current_offset_ns)

    exchange_id = random.choice(EXCHANGES)
    instrument = random.choice(objects)
    mtype = random.choice(msg_types)
    value = f"{random.uniform(10, 1000):.4f}"
    mcast_dst, dst_port = random.choice(MULTICAST_CHANNELS)
    dst_mac = multicast_ip_to_mac(mcast_dst)

    payload = f"{ts}|{exchange_id}|{instrument}|{mtype}|{value}".encode("ascii")

    pkt = (
        Ether(src=SRC_MAC, dst=dst_mac)
        / IP(src=f"10.10.0.{(i % 200) + 1}", dst=mcast_dst, ttl=16)
        / UDP(sport=30000 + (i % 1000), dport=dst_port)
        / Raw(load=payload)
    )
    packets.append(pkt)

wrpcap(OUT_FILE, packets)
if args.random:
    print(f"wrote {len(packets)} packets to {OUT_FILE} (mode=random)")
else:
    print(f"wrote {len(packets)} packets to {OUT_FILE} (mode=seed:{DEFAULT_SEED})")
