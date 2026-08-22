#include <gtest/gtest.h>
#include <curl/curl.h>

int main(int argc, char *argv[])
{
	testing::InitGoogleTest(&argc, argv);

	// The tests drive servers over libcurl and a cluster talks to etcd over it, so libcurl is
	// initialised once here rather than by whichever handle happens to be created first.
	curl_global_init(CURL_GLOBAL_DEFAULT);

	// Nothing calls curl_global_cleanup: a handle is held for the life of the thread that made
	// it, and the one belonging to this thread is closed after main has returned, which is after
	// anything here could have cleaned up under it. What libcurl holds is reachable, not lost.
	return RUN_ALL_TESTS();
}
