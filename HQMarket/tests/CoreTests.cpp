#include "../market/CMarketCache.h"
#include "../market/CSubscriptionManager.h"
#include "../network/CFrameCodec.h"
#include <cassert>
#include <iostream>
int main()
{
	net::CFrameCodec codec(1024);
	std::string encoded = net::CFrameCodec::Encode("payload");
	std::vector<std::string> frames;
	assert(codec.Append(encoded.data(), 2, frames));
	assert(frames.empty());
	assert(codec.Append(encoded.data() + 2, encoded.size() - 2, frames));
	assert((frames.size() == 1) && (frames[0] == "payload"));
	market::CSubscriptionManager subscriptions;
	market::CSubscription quote{{"600519", market::Exchange::sse}, market::Channel::quote};
	assert(subscriptions.Subscribe(1, {quote}).size() == 1);
	assert(subscriptions.Subscribe(2, {quote}).empty());
	assert(subscriptions.RemoveClient(1).empty());
	assert(subscriptions.RemoveClient(2).size() == 1);
	market::CMarketCache cache;
	market::CQuote value;
	value.m_instrument = quote.m_instrument;
	value.m_nLastPrice = 16880000;
	assert(cache.Update(value) == 1);
	assert(cache.GetQuote(value.m_instrument)->m_nLastPrice == 16880000);
	std::cout << "HQMarket core tests passed\n";
	return 0;
}
