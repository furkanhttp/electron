// Copyright (c) 2018 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/net/request_context_factory.h"

#include <algorithm>

#include "atom/browser/atom_browser_context.h"
#include "atom/browser/net/about_protocol_handler.h"
#include "atom/browser/net/asar/asar_protocol_handler.h"
#include "atom/browser/net/atom_cert_verifier.h"
#include "atom/browser/net/atom_network_delegate.h"
#include "atom/browser/net/atom_url_request_job_factory.h"
#include "atom/browser/net/cookie_details.h"
#include "atom/browser/net/http_protocol_handler.h"
#include "atom/common/options_switches.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task_scheduler/post_task.h"
#include "brightray/browser/browser_client.h"
#include "brightray/browser/browser_context.h"
#include "brightray/browser/net/require_ct_delegate.h"
#include "brightray/browser/net/url_request_context_getter_factory.h"
#include "brightray/browser/net_log.h"
#include "components/network_session_configurator/common/network_switches.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/cookie_store_factory.h"
#include "content/public/browser/devtools_network_transaction_factory.h"
#include "content/public/browser/resource_context.h"
#include "net/base/host_mapping_rules.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/ct_known_logs.h"
#include "net/cert/ct_log_verifier.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/multi_log_ct_verifier.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_store.h"
#include "net/dns/mapped_host_resolver.h"
#include "net/ftp/ftp_network_layer.h"
#include "net/http/http_auth_handler_factory.h"
#include "net/http/http_auth_preferences.h"
#include "net/http/http_cache.h"
#include "net/http/http_server_properties_impl.h"
#include "net/http/transport_security_state.h"
#include "net/proxy_resolution/dhcp_pac_file_fetcher_factory.h"
#include "net/proxy_resolution/pac_file_fetcher_impl.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/proxy_resolution/proxy_service.h"
#include "net/ssl/channel_id_service.h"
#include "net/ssl/default_channel_id_store.h"
#include "net/ssl/ssl_config_service_defaults.h"
#include "net/url_request/data_protocol_handler.h"
#include "net/url_request/file_protocol_handler.h"
#include "net/url_request/ftp_protocol_handler.h"
#include "net/url_request/static_http_user_agent_settings.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_context_storage.h"
#include "net/url_request/url_request_intercepting_job_factory.h"
#include "services/network/public/cpp/network_switches.h"

using content::BrowserThread;

namespace atom {

namespace {

class NoCacheBackend : public net::HttpCache::BackendFactory {
  int CreateBackend(net::NetLog* net_log,
                    std::unique_ptr<disk_cache::Backend>* backend,
                    const net::CompletionCallback& callback) override {
    return net::ERR_FAILED;
  }
};

net::HttpCache::BackendFactory* CreateHttpCacheBackendFactory(
    bool use_cache,
    const base::FilePath& base_path) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (!use_cache ||
      command_line->HasSwitch(atom::switches::kDisableHttpCache)) {
    return new NoCacheBackend;
  } else {
    int max_size = 0;
    base::StringToInt(
        command_line->GetSwitchValueASCII(atom::switches::kDiskCacheSize),
        &max_size);
    base::FilePath cache_path = base_path.Append(FILE_PATH_LITERAL("Cache"));
    return new net::HttpCache::DefaultBackend(
        net::DISK_CACHE, net::CACHE_BACKEND_DEFAULT, cache_path, max_size);
  }
}

std::unique_ptr<net::URLRequestJobFactory> CreateURLRequestJobFactory(
    content::ProtocolHandlerMap* protocol_handlers,
    net::HostResolver* host_resolver) {
  std::unique_ptr<AtomURLRequestJobFactory> job_factory(
      new AtomURLRequestJobFactory);

  for (auto& it : *protocol_handlers) {
    job_factory->SetProtocolHandler(it.first,
                                    base::WrapUnique(it.second.release()));
  }
  protocol_handlers->clear();

  job_factory->SetProtocolHandler(url::kAboutScheme,
                                  base::WrapUnique(new AboutProtocolHandler));
  job_factory->SetProtocolHandler(
      url::kDataScheme, base::WrapUnique(new net::DataProtocolHandler));
  job_factory->SetProtocolHandler(
      url::kFileScheme,
      base::WrapUnique(
          new asar::AsarProtocolHandler(base::CreateTaskRunnerWithTraits(
              {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
               base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}))));
  job_factory->SetProtocolHandler(
      url::kHttpScheme,
      base::WrapUnique(new HttpProtocolHandler(url::kHttpScheme)));
  job_factory->SetProtocolHandler(
      url::kHttpsScheme,
      base::WrapUnique(new HttpProtocolHandler(url::kHttpsScheme)));
  job_factory->SetProtocolHandler(
      url::kWsScheme,
      base::WrapUnique(new HttpProtocolHandler(url::kWsScheme)));
  job_factory->SetProtocolHandler(
      url::kWssScheme,
      base::WrapUnique(new HttpProtocolHandler(url::kWssScheme)));
  job_factory->SetProtocolHandler(
      url::kFtpScheme, net::FtpProtocolHandler::Create(host_resolver));

  return std::move(job_factory);
}

}  // namespace

AtomMainRequestContextFactory::AtomMainRequestContextFactory(
    const base::FilePath& path,
    bool in_memory,
    bool use_cache,
    std::string user_agent,
    std::vector<std::string> cookieable_schemes,
    content::ProtocolHandlerMap* protocol_handlers,
    content::URLRequestInterceptorScopedVector request_interceptors,
    base::WeakPtr<brightray::BrowserContext> browser_context)
    : base_path_(path),
      in_memory_(in_memory),
      use_cache_(use_cache),
      user_agent_(user_agent),
      cookieable_schemes_(cookieable_schemes),
      request_interceptors_(std::move(request_interceptors)),
      job_factory_(nullptr),
      browser_context_(browser_context),
      weak_ptr_factory_(this) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (protocol_handlers)
    std::swap(protocol_handlers_, *protocol_handlers);

  net_log_ = static_cast<brightray::NetLog*>(
      brightray::BrowserClient::Get()->GetNetLog());

  // We must create the proxy config service on the UI loop on Linux because it
  // must synchronously run on the glib message loop. This will be passed to
  // the URLRequestContextStorage on the IO thread in GetURLRequestContext().
  proxy_config_service_ =
      net::ProxyResolutionService::CreateSystemProxyConfigService(
          BrowserThread::GetTaskRunnerForThread(BrowserThread::IO));
}

AtomMainRequestContextFactory::~AtomMainRequestContextFactory() {
  weak_ptr_factory_.InvalidateWeakPtrs();
}

void AtomMainRequestContextFactory::OnCookieChanged(
    const net::CanonicalCookie& cookie,
    net::CookieChangeCause cause) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::BindOnce(&AtomMainRequestContextFactory::NotifyCookieChange,
                     weak_ptr_factory_.GetWeakPtr(), cookie, cause));
}

void AtomMainRequestContextFactory::NotifyCookieChange(
    const net::CanonicalCookie& cookie,
    net::CookieChangeCause cause) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  CookieDetails cookie_details(
      &cookie, !(cause == net::CookieChangeCause::INSERTED), cause);

  if (browser_context_)
    static_cast<AtomBrowserContext*>(browser_context_.get())
        ->NotifyCookieChange(&cookie_details);
}

net::URLRequestContext* AtomMainRequestContextFactory::Create() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  auto& command_line = *base::CommandLine::ForCurrentProcess();
  url_request_context_.reset(new net::URLRequestContext);

  ct_delegate_.reset(new brightray::RequireCTDelegate);

  // --log-net-log
  if (net_log_) {
    net_log_->StartLogging();
    url_request_context_->set_net_log(net_log_);
  }

  storage_.reset(new net::URLRequestContextStorage(url_request_context_.get()));

  storage_->set_network_delegate(std::make_unique<AtomNetworkDelegate>());

  auto cookie_path = in_memory_
                         ? base::FilePath()
                         : base_path_.Append(FILE_PATH_LITERAL("Cookies"));
  std::unique_ptr<net::CookieStore> cookie_store = content::CreateCookieStore(
      content::CookieStoreConfig(cookie_path, false, false, nullptr));
  storage_->set_cookie_store(std::move(cookie_store));

  // Set custom schemes that can accept cookies.
  net::CookieMonster* cookie_monster =
      static_cast<net::CookieMonster*>(url_request_context_->cookie_store());
  cookie_monster->SetCookieableSchemes(cookieable_schemes_);
  // Cookie store will outlive notifier by order of declaration
  // in the header.
  cookie_change_sub_ = url_request_context_->cookie_store()
                           ->GetChangeDispatcher()
                           .AddCallbackForAllChanges(base::Bind(
                               &AtomMainRequestContextFactory::OnCookieChanged,
                               base::Unretained(this)));

  storage_->set_channel_id_service(std::make_unique<net::ChannelIDService>(
      new net::DefaultChannelIDStore(nullptr)));

  storage_->set_http_user_agent_settings(
      base::WrapUnique(new net::StaticHttpUserAgentSettings(
          net::HttpUtil::GenerateAcceptLanguageHeader(
              brightray::BrowserClient::Get()->GetApplicationLocale()),
          user_agent_)));

  std::unique_ptr<net::HostResolver> host_resolver(
      net::HostResolver::CreateDefaultResolver(nullptr));

  // --host-resolver-rules
  if (command_line.HasSwitch(network::switches::kHostResolverRules)) {
    std::unique_ptr<net::MappedHostResolver> remapped_resolver(
        new net::MappedHostResolver(std::move(host_resolver)));
    remapped_resolver->SetRulesFromString(command_line.GetSwitchValueASCII(
        network::switches::kHostResolverRules));
    host_resolver = std::move(remapped_resolver);
  }

  // --proxy-server
  if (command_line.HasSwitch(switches::kNoProxyServer)) {
    storage_->set_proxy_resolution_service(
        net::ProxyResolutionService::CreateDirect());
  } else if (command_line.HasSwitch(switches::kProxyServer)) {
    net::ProxyConfig proxy_config;
    proxy_config.proxy_rules().ParseFromString(
        command_line.GetSwitchValueASCII(switches::kProxyServer));
    proxy_config.proxy_rules().bypass_rules.ParseFromString(
        command_line.GetSwitchValueASCII(switches::kProxyBypassList));
    storage_->set_proxy_resolution_service(
        net::ProxyResolutionService::CreateFixed(proxy_config));
  } else if (command_line.HasSwitch(switches::kProxyPacUrl)) {
    auto proxy_config = net::ProxyConfig::CreateFromCustomPacURL(
        GURL(command_line.GetSwitchValueASCII(switches::kProxyPacUrl)));
    proxy_config.set_pac_mandatory(true);
    storage_->set_proxy_resolution_service(
        net::ProxyResolutionService::CreateFixed(proxy_config));
  } else {
    storage_->set_proxy_resolution_service(
        net::ProxyResolutionService::CreateUsingSystemProxyResolver(
            std::move(proxy_config_service_), net_log_));
  }

  std::vector<std::string> schemes;
  schemes.push_back(std::string("basic"));
  schemes.push_back(std::string("digest"));
  schemes.push_back(std::string("ntlm"));
  schemes.push_back(std::string("negotiate"));
#if defined(OS_POSIX)
  http_auth_preferences_.reset(
      new net::HttpAuthPreferences(schemes, std::string()));
#else
  http_auth_preferences_.reset(new net::HttpAuthPreferences(schemes));
#endif

  // --auth-server-whitelist
  if (command_line.HasSwitch(switches::kAuthServerWhitelist)) {
    http_auth_preferences_->SetServerWhitelist(
        command_line.GetSwitchValueASCII(switches::kAuthServerWhitelist));
  }

  // --auth-negotiate-delegate-whitelist
  if (command_line.HasSwitch(switches::kAuthNegotiateDelegateWhitelist)) {
    http_auth_preferences_->SetDelegateWhitelist(
        command_line.GetSwitchValueASCII(
            switches::kAuthNegotiateDelegateWhitelist));
  }

  auto auth_handler_factory = net::HttpAuthHandlerRegistryFactory::Create(
      http_auth_preferences_.get(), host_resolver.get());

  std::unique_ptr<net::TransportSecurityState> transport_security_state =
      std::make_unique<net::TransportSecurityState>();
  transport_security_state->SetRequireCTDelegate(ct_delegate_.get());
  storage_->set_transport_security_state(std::move(transport_security_state));
  storage_->set_cert_verifier(
      std::make_unique<AtomCertVerifier>(ct_delegate_.get()));
  storage_->set_ssl_config_service(new net::SSLConfigServiceDefaults);
  storage_->set_http_auth_handler_factory(std::move(auth_handler_factory));
  std::unique_ptr<net::HttpServerProperties> server_properties(
      new net::HttpServerPropertiesImpl);
  storage_->set_http_server_properties(std::move(server_properties));

  std::unique_ptr<net::MultiLogCTVerifier> ct_verifier =
      std::make_unique<net::MultiLogCTVerifier>();
  ct_verifier->AddLogs(net::ct::CreateLogVerifiersForKnownLogs());
  storage_->set_cert_transparency_verifier(std::move(ct_verifier));
  storage_->set_ct_policy_enforcer(std::make_unique<net::CTPolicyEnforcer>());

  net::HttpNetworkSession::Params network_session_params;
  network_session_params.ignore_certificate_errors = false;

  // --disable-http2
  if (command_line.HasSwitch(switches::kDisableHttp2))
    network_session_params.enable_http2 = false;

  // --ignore-certificate-errors
  if (command_line.HasSwitch(::switches::kIgnoreCertificateErrors))
    network_session_params.ignore_certificate_errors = true;

  // --host-rules
  if (command_line.HasSwitch(switches::kHostRules)) {
    host_mapping_rules_.reset(new net::HostMappingRules);
    host_mapping_rules_->SetRulesFromString(
        command_line.GetSwitchValueASCII(switches::kHostRules));
    network_session_params.host_mapping_rules = *host_mapping_rules_.get();
  }

  // Give |storage_| ownership at the end in case it's |mapped_host_resolver|.
  storage_->set_host_resolver(std::move(host_resolver));

  net::HttpNetworkSession::Context network_session_context;
  net::URLRequestContextBuilder::SetHttpNetworkSessionComponents(
      url_request_context_.get(), &network_session_context);
  http_network_session_.reset(new net::HttpNetworkSession(
      network_session_params, network_session_context));

  std::unique_ptr<net::HttpCache::BackendFactory> backend;
  if (in_memory_) {
    backend = net::HttpCache::DefaultBackend::InMemory(0);
  } else {
    backend.reset(CreateHttpCacheBackendFactory(use_cache_, base_path_));
  }

  storage_->set_http_transaction_factory(std::make_unique<net::HttpCache>(
      content::CreateDevToolsNetworkTransactionFactory(
          http_network_session_.get()),
      std::move(backend), false));

  std::unique_ptr<net::URLRequestJobFactory> job_factory =
      CreateURLRequestJobFactory(&protocol_handlers_,
                                 url_request_context_->host_resolver());
  job_factory_ = job_factory.get();

  // Set up interceptors in the reverse order.
  std::unique_ptr<net::URLRequestJobFactory> top_job_factory =
      std::move(job_factory);
  if (!request_interceptors_.empty()) {
    for (auto it = request_interceptors_.rbegin();
         it != request_interceptors_.rend(); ++it) {
      top_job_factory.reset(new net::URLRequestInterceptingJobFactory(
          std::move(top_job_factory), std::move(*it)));
    }
    request_interceptors_.clear();
  }

  storage_->set_job_factory(std::move(top_job_factory));

  return url_request_context_.get();
}

}  // namespace atom
