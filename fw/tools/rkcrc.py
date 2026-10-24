"""CRC32 of RKnano firmware images (polynomial 0x04C10DB7, MSB first, initial value 0), as the
IDB loader and the stock firmware check it at the end of an image (ETUDE §2)."""

_T = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C10DB7) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _T.append(_c)


def rkcrc(data, crc=0):
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _T[((crc >> 24) ^ b) & 0xFF]
    return crc
