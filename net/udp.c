#include <levos/kernel.h>
#include <levos/packet.h>
#include <levos/udp.h>
#include <levos/ip.h>
#include <levos/eth.h>
#include <levos/arp.h>
#include <levos/dhcp.h>
#include <levos/work.h>
#include <levos/socket.h>

void
udp_write_header(struct udp_header *udp, port_t srcport, port_t dstport)
{
    udp->udp_src_port = to_be_16(srcport);
    udp->udp_dst_port = to_be_16(dstport);
    udp->udp_len = to_be_16(8); /* minimum */
    udp->udp_chksum = 0; /* TODO: do checksum */
}

uint16_t
udp_calc_checksum(struct ip_base_header *ip, struct udp_header *udp, void *payload, size_t payload_len)
{
    uint32_t sum = 0;
    int i = 0;

    uint16_t *ptr = (uint16_t *) &ip->ip_srcaddr;
    sum += ptr[0];
    sum += ptr[1];

    ptr = (uint16_t *) &ip->ip_dstaddr;
    sum += ptr[0];
    sum += ptr[1];

    sum += 0;
    sum += ip->ip_proto;

    sum += udp->udp_len;

    sum += udp->udp_src_port;
    sum += udp->udp_dst_port;

    sum += udp->udp_len;

    ptr = payload;
    for (i = 0; i < payload_len / sizeof(uint16_t); i++)
        sum += ptr[i];

    while (sum > 0xffff) {
        uint16_t val = sum & 0xffff0000;
        sum &= 0x0000ffff;
        sum += val;
    }

    return sum;
}

int udp_add_header(packet_t *pkt, port_t srcport, port_t dstport)
{
    if (packet_grow(pkt, sizeof(struct udp_header)))
        return -ENOMEM;

    udp_write_header(pkt->p_ptr, srcport, dstport);

    return 0;
}

int
udp_set_payload(packet_t *pkt, void *data, size_t len)
{
    struct udp_header *udp;
    struct ip_base_header *ip;

    /* first, grow the packet */
    if (packet_grow(pkt, len))
        return -ENOMEM;

    /* add the payload */
    memcpy(pkt->p_ptr, data, len);

    /* update UDP header */
    udp = pkt->p_buf + pkt->pkt_proto_offset;
    udp->udp_len = to_be_16(sizeof(struct udp_header) + len);
    //udp->udp_chksum = udp_calc_checksum(pkt->p_buf + pkt->pkt_ip_offset, udp, data, len);
    udp->udp_chksum = 0;

    /* update the IP header */
    ip = pkt->p_buf + pkt->pkt_ip_offset;
    ip_update_length(ip, sizeof(struct udp_header) + len);

    return 0;
}

packet_t *udp_construct_packet(struct net_info *ni, 
                               eth_addr_t srceth, ip_addr_t srcip, port_t srcport,
                               ip_addr_t dstip, port_t dstport)
{
    int rc;
    packet_t *pkt;
    uint8_t *dsteth;

    /* FIXME: ouch */
    dstip = to_be_32(dstip);

    dsteth = arp_get_eth_addr(ni, dstip);

    if (dsteth == NULL)
        return NULL;

    pkt = ip_construct_packet_eth_full(srceth, dsteth, srcip, dstip);
    if (!pkt)
        return NULL;

    ip_set_proto(pkt->p_buf + pkt->pkt_ip_offset, IP_PROTO_UDP);

    rc = udp_add_header(pkt, srcport, dstport);
    if (rc)
        return NULL;

    pkt->pkt_proto_offset = pkt->p_ptr - pkt->p_buf;

    //udp_set_payload(pkt, udp_dummy_data, strlen(udp_dummy_data));

    return pkt;
}

packet_t *udp_new_packet(struct net_info *ni, port_t srcport, ip_addr_t dstip,
                            port_t dstport)
{
    return udp_construct_packet(ni, ni->ni_hw_mac, ni->ni_src_ip, srcport,
                                dstip, dstport);
}

int
udp_handle_packet(struct net_info *ni, packet_t *pkt, struct udp_header *udp)
{
    net_printk(" ^ udp\n");
    pkt->pkt_proto_offset = (int)udp - (int)pkt->p_buf;
    pkt->p_ptr += sizeof(struct udp_header);
    /* try figuring out where the UDP packet is headed */
    if (udp->udp_dst_port == to_be_16(68) &&
            udp->udp_src_port == to_be_16(67)) {
        return dhcp_handle_packet(ni, pkt, udp);
    } else {
        net_printk("  ^ application handler null for port %d\n", udp->udp_dst_port);
    }
    return PACKET_DROP;
}

int
do_udp_inet_sock_connect(struct socket *sock, struct sockaddr_in *addr, socklen_t len)
{
    struct udp_sock_priv *priv = sock->sock_priv;

    sock->sock_ni = route_find_ni_for_dst(addr->sin_addr);

    priv->usp_dstip = to_le_32(addr->sin_addr);
    priv->usp_dstport = to_le_16(addr->sin_port);
    priv->usp_srcport = net_allocate_port(SOCK_DGRAM);

    net_printk("connected a UDP socket to %pI dstport %d srcport %d\n",
            priv->usp_dstip, priv->usp_dstport, priv->usp_srcport);

    return 0;
}

int
udp_sock_connect(struct socket *sock, struct sockaddr *addr, socklen_t len)
{
    sock->sock_priv = malloc(sizeof(struct udp_sock_priv));
    if (!sock->sock_priv)
        return -ENOMEM;

    if (sock->sock_domain == AF_INET)
        return do_udp_inet_sock_connect(sock, (struct sockaddr_in *) addr, len);

    printk("WARNING: trying to use UDP on not AF_INET!\n");
    return -EINVAL;
}

int
udp_sock_write(struct socket *sock, void *buf, size_t len)
{
    struct udp_sock_priv *priv = sock->sock_priv;
    struct net_device *ndev = NDEV_FROM_NI(sock->sock_ni);
    packet_t *pkt;

    if (priv == NULL)
        return -ENOTCONN;

    net_printk("UDP socket write! from %pI:%d to %pI:%d\n", sock->sock_ni->ni_src_ip,
            priv->usp_srcport, priv->usp_dstip, priv->usp_dstport);

    pkt = udp_new_packet(sock->sock_ni, priv->usp_srcport, priv->usp_dstip,
                            priv->usp_dstport);
    if (!pkt)
        return -ENOMEM;

    udp_set_payload(pkt, buf, len);

    ndev->send_packet(ndev, pkt);

    return 0;
}

int
udp_sock_destroy(struct socket *sock)
{
    struct udp_sock_priv *priv = sock->sock_priv;

    if (priv == NULL)
        return 0;

    net_free_port(SOCK_DGRAM, priv->usp_srcport);

    free(priv);

    return 0;
}

struct socket_ops udp_sock_ops = {
    .connect = udp_sock_connect,
    .write = udp_sock_write,
    .destroy = udp_sock_destroy,
};
