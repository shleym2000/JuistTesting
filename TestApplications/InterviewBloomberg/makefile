APP = bloomberg

bloomberg: $(APP).cpp
	g++ -O3 $(APP).cpp -o $(APP)

clean:
	rm -f $(APP)

all: bloomberg

run: all
	./$(APP) ./synthetic_market_multicast.pcap
