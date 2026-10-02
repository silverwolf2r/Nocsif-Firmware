cmake_minimum_required(VERSION 3.16.0)
# WiFi #6 Network Tools — Discover Hosts reads the lwIP ARP table to list every device on the /24
# (not just the few that answer ICMP echo). lwIP's ARP_TABLE_SIZE (lwip/opt.h) defaults to 10 with no
# Kconfig hook, so a .1->.254 sweep would LRU-evict earlier neighbours; raise it so a whole home LAN
# fits. Global so it reaches the lwip component (nettools.c also captures per-batch as a backstop).
add_compile_definitions(ARP_TABLE_SIZE=64)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)
