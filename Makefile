CXXFLAGS += -lwiringPi

default: RCSwitch.cpp server.cpp
	$(CXX) $+ -o server $(CXXFLAGS) $(LDFLAGS)
