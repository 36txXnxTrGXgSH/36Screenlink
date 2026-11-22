#pragma once
#include <string>

#include <vector>

// Cherche les appareils par mDNS (_screenlink._tcp) pendant timeoutMs. Renvoie leurs adresses IPv4.
std::vector<std::string> discoverDevices(int timeoutMs);
