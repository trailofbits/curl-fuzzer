# syntax=docker/dockerfile:1

# Use the following commands to run fuzzing:
# $ docker buildx build -t curl_fuzz .
# $ docker run --rm -it --cap-add=NET_ADMIN curl_fuzz

FROM aflplusplus/aflplusplus

RUN apt-get update && apt-get install -y libssl-dev netcat iptables groff

# Clone a curl repository
RUN git clone https://github.com/curl/curl.git

# Apply a patch to use afl++ argv fuzzing feature
COPY <<-EOT /AFLplusplus/curl/curl_argv_fuzz.patch
		diff --git a/src/tool_main.c b/src/tool_main.c
		--- a/src/tool_main.c
		+++ b/src/tool_main.c
		@@ -54,6 +54,7 @@
		 #include "tool_vms.h"
		 #include "tool_main.h"
		 #include "tool_libinfo.h"
		+#include "../../AFLplusplus/utils/argv_fuzzing/argv-fuzz-inl.h"

		 /*
		  * This is low-level hard-hacking memory leak tracking and similar. Using
		@@ -246,6 +247,8 @@ int main(int argc, char *argv[])
		   struct GlobalConfig global;
		   memset(&global, 0, sizeof(global));

		+  AFL_INIT_ARGV();
		+
		 #ifdef WIN32
		   /* Undocumented diagnostic option to list the full paths of all loaded
		      modules. This is purposely pre-init. */
EOT

# Apply a patch to use afl++ argv fuzzing feature
RUN cd curl && git apply curl_argv_fuzz.patch

# Compile a curl using collision-free instrumentation at link time and ASAN
RUN cd curl && \
	autoreconf -i && \
	CC="afl-clang-lto" CFLAGS="-fsanitize=address -g" ./configure --with-openssl --disable-shared && \
	make -j $(nproc) && \
	make install

# Download a dictionary
RUN wget https://gist.githubusercontent.com/ahpaleus/f94eca6b29ca8824cf6e5a160379612b/raw/3de91b2dfc5ddd8b4b2357b0eb7fbcdc257384c4/curl.dict

COPY <<-EOT script.sh
	#!/bin/bash
	# Running a netcat listener on port tcp port 80 in the background
	netcat -l 80 -k -w 0 &

	# Prepare iptables entries
	iptables-legacy -t nat -A OUTPUT -p tcp -j REDIRECT --to-port 80
	iptables-legacy -t nat -A OUTPUT -p udp --dport 53 -j DNAT --to-destination 127.0.0.1

	# Prepare fuzzing directories
	mkdir fuzz &&
		  cd fuzz &&
		  mkdir in out &&
		  echo -ne 'curl\x00http://127.0.0.1:80' > in/example_command.txt &&
		  # Run afl++ fuzzer
		  afl-fuzz -x /AFLplusplus/curl.dict -i in/ -o out/ -- curl
EOT

RUN chmod +x ./script.sh
ENTRYPOINT ["./script.sh"]

