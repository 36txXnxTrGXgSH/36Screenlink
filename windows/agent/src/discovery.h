#pragma once
#include <string>

// Cherche la tablette par mDNS (_screenlink._tcp). Renvoie son adresse IPv4, ou vide.
std::string discoverTablet(int timeoutMs);
