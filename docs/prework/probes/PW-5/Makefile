PXR ?= $USD
SCHEMA_RES ?= <usdgen-src>/build/usd/usdGenSchema/resources
IMAGING_RES ?= <usdgen-src>/build/usd/usdGenImaging/resources

probe: probe.cpp
	g++ -std=c++17 -O1 -w $< -o $@ -I $(PXR)/include -I /usr/include/python3.12 \
	  -L $(PXR)/lib \
	  -lusd_usd -lusd_usdGeom -lusd_tf -lusd_sdf -lusd_gf -lusd_arch \
	  -lusd_kind -lusd_vt -lusd_trace -lusd_work -lusd_js -lusd_plug \
	  -lusd_ar -lusd_hio -lusd_usdImaging -lusd_hd -lusd_hdsi -lusd_python \
	  -Wl,-rpath,$(PXR)/lib

run: probe
	LD_LIBRARY_PATH=$(PXR)/lib \
	PXR_PLUGINPATH_NAME="$(SCHEMA_RES):$(IMAGING_RES):$(PXR)/plugin/usd" \
	./probe 2>&1 | tee run1.txt

clean:
	rm -f probe run1.txt

.PHONY: run clean
