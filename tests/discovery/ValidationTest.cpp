#include <gtest/gtest.h>

#include <string>

#include "edgeflow/discovery/Validation.hpp"

namespace {

using namespace edgeflow::discovery;

TEST(ValidationTest, ServiceNames) {
  for (const char* ok : {"user-service", "a", "svc.v2", "my_service", "a1", "0abc9"}) {
    EXPECT_FALSE(validateServiceName(ok)) << ok;
  }
  for (const char* bad : {"", "User", "-svc", "svc-", ".svc", "svc/x", "svc x", "svc;drop", "ü", "a b"}) {
    EXPECT_TRUE(validateServiceName(bad)) << bad;
  }
  EXPECT_FALSE(validateServiceName(std::string(64, 'a')));
  EXPECT_TRUE(validateServiceName(std::string(65, 'a')));
}

TEST(ValidationTest, InstanceIds) {
  for (const char* ok : {"i-1", "Instance.A_2", "x"}) EXPECT_FALSE(validateInstanceId(ok)) << ok;
  for (const char* bad : {"", "a b", "a/b", "a:b", "a'b", "é"}) EXPECT_TRUE(validateInstanceId(bad)) << bad;
  EXPECT_FALSE(validateInstanceId(std::string(128, 'a')));
  EXPECT_TRUE(validateInstanceId(std::string(129, 'a')));
}

TEST(ValidationTest, Hosts) {
  for (const char* ok : {"10.0.0.11", "127.0.0.1", "::1", "2001:db8::1", "backend", "user-service.internal",
                         "a.b.c", "host-1"}) {
    EXPECT_FALSE(validateHost(ok)) << ok;
  }
  for (const char* bad : {"", "-host", "host-", "a..b", ".a", "a.", "host name", "http://x", "host:80",
                          "host/path", "a_b", "'; DROP TABLE x; --"}) {
    EXPECT_TRUE(validateHost(bad)) << bad;
  }
  EXPECT_TRUE(validateHost(std::string(254, 'a')));
  EXPECT_TRUE(validateHost(std::string(64, 'a')));  // single label longer than 63
}

TEST(ValidationTest, PortWeightVersionAndConnectionCount) {
  EXPECT_TRUE(validatePort(0));
  EXPECT_FALSE(validatePort(1));
  EXPECT_FALSE(validatePort(65535));
  EXPECT_TRUE(validatePort(65536));

  EXPECT_FALSE(validateWeight(0));
  EXPECT_FALSE(validateWeight(1000));
  EXPECT_TRUE(validateWeight(1001));

  EXPECT_FALSE(validateVersion(""));
  EXPECT_FALSE(validateVersion("1.4.2-rc1 (build 7)"));
  EXPECT_TRUE(validateVersion(std::string(65, 'v')));
  EXPECT_TRUE(validateVersion(std::string("a\nb")));

  EXPECT_FALSE(validateConnectionCount(0));
  EXPECT_FALSE(validateConnectionCount(9223372036854775807ULL));
  EXPECT_TRUE(validateConnectionCount(9223372036854775808ULL));
}

TEST(ValidationTest, WholeObjectsAreChecked) {
  NewInstance good;
  good.service = "svc";
  good.host = "10.0.0.1";
  good.port = 9000;
  EXPECT_FALSE(validate(good));

  auto bad = good;
  bad.port = 0;
  EXPECT_TRUE(validate(bad));
  bad = good;
  bad.instance_id = "bad id";
  EXPECT_TRUE(validate(bad));
  bad = good;
  bad.weight = 5000;
  EXPECT_TRUE(validate(bad));

  InstanceUpdate empty;
  EXPECT_TRUE(validate(empty));
  InstanceUpdate update;
  update.weight = 10;
  EXPECT_FALSE(validate(update));
  update.weight = 1001;
  EXPECT_TRUE(validate(update));
}

TEST(ServiceInstanceTest, EnumsRoundTrip) {
  for (auto s : {InstanceStatus::Active, InstanceStatus::Draining, InstanceStatus::Disabled}) {
    EXPECT_EQ(parseInstanceStatus(toString(s)), s);
  }
  for (auto h : {HealthStatus::Unknown, HealthStatus::Healthy, HealthStatus::Unhealthy}) {
    EXPECT_EQ(parseHealthStatus(toString(h)), h);
  }
  EXPECT_FALSE(parseInstanceStatus("healthy"));  // health and status are distinct vocabularies
  EXPECT_FALSE(parseHealthStatus("active"));
  EXPECT_FALSE(parseInstanceStatus("ACTIVE"));
}

}  // namespace
