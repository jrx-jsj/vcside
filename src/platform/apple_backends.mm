// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "assistant/http.h"
#include "assistant/secrets.h"
#include "platform/apple_backends.h"

// ---- HTTPS: one NSURLSession task per request, driven from the worker thread --------------

@interface VcsStream : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) const std::function<bool(const char*, size_t)>* onBody;
@property(nonatomic) vcs::assistant::HttpResult* result;
@property(nonatomic) dispatch_semaphore_t finished;
@property(nonatomic) BOOL success;
@property(nonatomic) BOOL stoppedByCaller;
@end

@implementation VcsStream
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))handler {
  self.result->status = (int)[(NSHTTPURLResponse*)response statusCode];
  self.success = self.result->status / 100 == 2;
  NSString* retryAfter = [(NSHTTPURLResponse*)response valueForHTTPHeaderField:@"Retry-After"];
  if (retryAfter) self.result->retryAfterSeconds = [retryAfter doubleValue];
  handler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
  if (self.success) {
    if (!(*self.onBody)((const char*)data.bytes, data.length)) {
      self.stoppedByCaller = YES;
      [task cancel];
    }
  } else if (self.result->errorBody.size() < 4096) {
    self.result->errorBody.append((const char*)data.bytes, std::min<size_t>(data.length, 4096 - self.result->errorBody.size()));
  }
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  if (error && !self.stoppedByCaller && error.code != NSURLErrorCancelled) self.result->error = [error.localizedDescription UTF8String];
  dispatch_semaphore_signal(self.finished);
}
@end

static vcs::assistant::HttpResult appleHttps(const vcs::assistant::HttpRequest& request, const std::function<bool(const char*, size_t)>& onBody,
                                              const std::atomic<bool>& cancel) {
  using namespace vcs::assistant;
  HttpResult result;
  @autoreleasepool {
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
    if (!url) {
      result.error = "invalid URL";
      return result;
    }
    NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
    urlRequest.HTTPMethod = [NSString stringWithUTF8String:request.method.c_str()];
    urlRequest.timeoutInterval = request.idleSeconds;  // no data for this long (the session below resets it per chunk)
    [urlRequest setValue:@"VCSIDE" forHTTPHeaderField:@"User-Agent"];
    for (auto& header : request.headers)
      [urlRequest setValue:[NSString stringWithUTF8String:header.second.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:header.first.c_str()]];
    if (!request.body.empty()) urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];
    NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];  // no cookies, no cache on disk
    configuration.timeoutIntervalForRequest = request.idleSeconds;
    configuration.timeoutIntervalForResource = 3600;
    VcsStream* stream = [VcsStream new];
    stream.onBody = &onBody;
    stream.result = &result;
    stream.finished = dispatch_semaphore_create(0);
    NSOperationQueue* queue = [NSOperationQueue new];
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration delegate:stream delegateQueue:queue];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:urlRequest];
    [task resume];
    bool cancelled = false;
    while (dispatch_semaphore_wait(stream.finished, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
      if (cancel && !cancelled) {
        cancelled = true;
        [task cancel];
      }
    }
    [session finishTasksAndInvalidate];
    if (cancelled) result.error = "cancelled";
  }
  return result;
}

// ---- the Keychain ---------------------------------------------------------------------------

static NSDictionary* keychainQuery(const std::string& account) {
  return @{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
           (__bridge id)kSecAttrService : @"VCSIDE assistant",
           (__bridge id)kSecAttrAccount : [NSString stringWithUTF8String:account.c_str()]};
}

static bool keychainLoad(const std::string& account, std::string* secret) {
  NSMutableDictionary* query = [keychainQuery(account) mutableCopy];
  query[(__bridge id)kSecReturnData] = @YES;
  query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef data = nullptr;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &data) != errSecSuccess || !data) return false;
  NSData* bytes = (__bridge_transfer NSData*)data;
  secret->assign((const char*)bytes.bytes, bytes.length);
  return true;
}

static bool keychainRemove(const std::string& account) { return SecItemDelete((__bridge CFDictionaryRef)keychainQuery(account)) == errSecSuccess; }

static bool keychainSave(const std::string& account, const std::string& secret) {
  keychainRemove(account);
  NSMutableDictionary* item = [keychainQuery(account) mutableCopy];
  item[(__bridge id)kSecValueData] = [NSData dataWithBytes:secret.data() length:secret.size()];
  return SecItemAdd((__bridge CFDictionaryRef)item, nullptr) == errSecSuccess;
}

namespace vcs::assistant {
void registerAppleBackends() {
  static const SecretBackend keychain = {"macOS Keychain", keychainLoad, keychainSave, keychainRemove};
  setSecretBackend(&keychain);
  setHttpsBackend(appleHttps);
}
}  // namespace vcs::assistant
