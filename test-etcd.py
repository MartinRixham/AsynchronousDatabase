import grpc
import socket
from etcd3.etcdrpc import KVStub, RangeRequest

def test_etcd(elb_hostname: str):
    # Resolve ELB hostname to IPv4 manually
    ip = socket.gethostbyname(elb_hostname)
    target = f"{ip}:2379"
    print(f"Connecting to {target}")

    # Connect directly to the IP
    channel = grpc.insecure_channel(target)

    kv = KVStub(channel)
    request = RangeRequest(key=b"test")
    response = kv.Range(request, timeout=5)

    print("Cluster responded OK")
    print("KVs returned:", len(response.kvs))

if __name__ == "__main__":
    elb = "asyncd-EtcdN-On7N9ya3YCJL-caba21a201f8acff.elb.eu-west-2.amazonaws.com"
    test_etcd(elb)

