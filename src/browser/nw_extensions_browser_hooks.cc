#include "nw_extensions_browser_hooks.h"

#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_operations.h"

// base
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

#if defined(OS_WIN)
#include "base/win/scoped_gdi_object.h"
#include "ui/gfx/icon_util.h"
#endif

// content
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_features.h"

// content/nw/
#include "content/nw/src/api/menu/menu.h"
#include "content/nw/src/browser/nw_content_browser_hooks.h"
#include "content/nw/src/common/shell_switches.h"
#include "content/nw/src/nw_base.h"
#include "content/nw/src/nw_content_verifier_delegate.h"

// extensions
#include "extensions/browser/app_window/app_window.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "extensions/browser/guest_view/web_view/web_view_renderer_state.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_handlers/webview_info.h"
#include "extensions/common/manifest_url_handlers.h"

#if defined(OS_WIN)
#define _USE_MATH_DEFINES
#include <math.h>
#endif

using content::RenderProcessHost;

using extensions::ContentVerifierDelegate;
using extensions::Extension;
using extensions::ExtensionRegistry;
using extensions::NWContentVerifierDelegate;
using extensions::WebviewInfo;
using extensions::WebViewGuest;

namespace manifest_keys = extensions::manifest_keys;

namespace nw {

namespace {

#if 0
void AmendManifestContentScriptList(base::DictionaryValue* manifest,
                                    const std::string& name,
                                    const std::string& run_at) {
  base::ListValue* scripts = NULL;
  base::Value* temp_value = NULL;
  bool amend = false;

  if (manifest->Get(manifest_keys::kContentScripts, &temp_value))
      if (temp_value->GetAsList(&scripts))
        amend = true;
  if (!scripts)
    scripts = new base::ListValue();

  std::string js;
  if (manifest->GetString(name, &js)) {
    base::ListValue* js_list = new base::ListValue();
    js_list->Append(new base::StringValue(js));

    base::ListValue* matches = new base::ListValue();
    matches->Append(new base::StringValue("<all_urls>"));

    base::ListValue* exclude_matches = new base::ListValue();
    exclude_matches->Append(new base::StringValue("*://*/_generated_background_page.html"));

    base::DictionaryValue* content_script = new base::DictionaryValue();
    content_script->Set("js", js_list);
    content_script->Set("matches", matches);
    content_script->Set("exclude_matches", exclude_matches);
    content_script->SetString("run_at", run_at);
    content_script->SetBoolean("in_main_world", true);

    scripts->Append(content_script);

    if (!amend)
      manifest->Set(manifest_keys::kContentScripts, scripts);
  }
}
#endif

void AmendManifestStringList(base::DictionaryValue* manifest,
                   const std::string& path,
                   const std::string& string_value) {
  base::ListValue* pattern_list = NULL;
  base::Value* temp_pattern_value = NULL;
  bool amend = false;

  if (manifest->Get(path, &temp_pattern_value))
      if (temp_pattern_value->GetAsList(&pattern_list))
        amend = true;
  if (!pattern_list)
    pattern_list = new base::ListValue();

  pattern_list->Append(base::WrapUnique(new base::Value(string_value)));
  if (!amend)
    manifest->Set(path, base::WrapUnique(pattern_list));
}

void AmendManifestList(base::DictionaryValue* manifest,
                   const std::string& path,
                   const base::ListValue& list_value) {
  base::ListValue* pattern_list = NULL;

  if (manifest->GetList(path, &pattern_list)) {
    base::ListValue::const_iterator it;
    for(it = list_value.GetList().begin(); it != list_value.GetList().end(); ++it) {
      pattern_list->Append((*it).CreateDeepCopy());
    }
  } else {
    manifest->Set(path, list_value.CreateDeepCopy());
  }
}

std::unique_ptr<base::DictionaryValue> MergeManifest(const std::string& in_manifest) {
  // Following attributes will not be inherited from package.json 
  // Keep this list consistent with documents in `Manifest Format.md`
  static std::vector<const char*> non_inherited_attrs = {
                                                    switches::kmFullscreen,
                                                    switches::kmKiosk,
                                                    switches::kmPosition,
                                                    switches::kmResizable,
                                                    switches::kmShow
                                                    };
  std::unique_ptr<base::DictionaryValue> manifest;

  // retrieve `window` manifest set by `new-win-policy`
  std::string manifest_str = in_manifest.empty() ? base::UTF16ToUTF8(nw::GetCurrentNewWinManifest())
    : in_manifest;
  std::unique_ptr<base::Value> val(base::JSONReader::ReadDeprecated(manifest_str));
  if (val && val->is_dict()) {
    manifest.reset(static_cast<base::DictionaryValue*>(val.release()));
  } else {
    manifest.reset(new base::DictionaryValue());
  }

  // merge with default `window` manifest in package.json if exists
  nw::Package* pkg = nw::package();
  if (pkg) {
    std::string js_doc_start, js_doc_end;
    pkg->root()->GetString(::switches::kmInjectJSDocStart, &js_doc_start);
    if (!js_doc_start.empty())
      manifest->SetString(::switches::kmInjectJSDocStart, js_doc_start);
    pkg->root()->GetString(::switches::kmInjectJSDocEnd, &js_doc_end);
    if (!js_doc_end.empty())
      manifest->SetString(::switches::kmInjectJSDocEnd, js_doc_end);
    base::DictionaryValue* manifest_window = pkg->window();
    if (manifest_window) {
      std::unique_ptr<base::DictionaryValue> manifest_window_cloned = manifest_window->DeepCopyWithoutEmptyChildren();
      // filter out non inherited attributes
      std::vector<const char*>::iterator it;
      for(it = non_inherited_attrs.begin(); it != non_inherited_attrs.end(); it++) {
        manifest_window_cloned->RemoveWithoutPathExpansion(*it, NULL);
      }
      // overwrite default `window` manifest with the one passed by `new-win-policy`
      manifest_window_cloned->MergeDictionary(manifest.get());
      return manifest_window_cloned;
    }
  }

  return manifest;
}

}

void SetAppIcon(gfx::Image &app_icon);

#if defined(OS_WIN)
void SetAppHIcon(base::win::ScopedHICON icon);
void SetWindowHIcon(base::win::ScopedHICON icon);
#endif // OS_WIN

// browser

bool RphGuestFilterURLHook(RenderProcessHost* rph, const GURL* url)  {
  extensions::WebViewRendererState* renderer_state =
      extensions::WebViewRendererState::GetInstance();
  std::string owner_extension;
  int process_id = rph->GetID();
  if (!renderer_state->GetOwnerInfo(process_id, nullptr, &owner_extension))
    return false;
  const Extension* extension =
    ExtensionRegistry::Get(rph->GetBrowserContext())->enabled_extensions().GetByID(owner_extension);
  if (!extension)
    return false;
  bool file_scheme = false;
  if (WebviewInfo::IsURLWebviewAccessible(extension,
                                          WebViewGuest::GetPartitionID(rph),
                                          *url, &file_scheme)) {
    if (file_scheme) {
      content::ChildProcessSecurityPolicy::GetInstance()->GrantRequestScheme(
          process_id, url::kFileScheme);
    }
    return true;
  }
  return false;
}

typedef bool (*RphGuestFilterURLHookFn)(content::RenderProcessHost* rph, const GURL* url);
CONTENT_EXPORT extern RphGuestFilterURLHookFn gRphGuestFilterURLHook;

bool GuestSwapProcessHook(content::BrowserContext* browser_context, const GURL& url) {
  if (!url.SchemeIs("chrome-extension"))
    return false;
  ExtensionRegistry* registry = ExtensionRegistry::Get(browser_context);
  std::string extension_id = url.host();
  const Extension* extension = registry->enabled_extensions().GetByID(extension_id);
  if (extension && !extensions::ManifestURL::Get(extension, "devtools_page").is_empty())
    return false;
  return false;
}

typedef bool(*GuestSwapProcessHookFn)(content::BrowserContext*, const GURL& url);
CONTENT_EXPORT extern GuestSwapProcessHookFn gGuestSwapProcessHook;

void LoadNWAppAsExtensionHook(base::DictionaryValue* manifest,
                              const base::FilePath& extension_path,
                              std::string* error) {
  gRphGuestFilterURLHook = RphGuestFilterURLHook;
  gGuestSwapProcessHook = GuestSwapProcessHook;
  if (!manifest)
    return;

  std::string main_url, bg_script, icon_path;
  base::Value *node_remote = NULL;

  const base::CommandLine* cmdline = base::CommandLine::ForCurrentProcess();
  nw::Package* package = cmdline->HasSwitch("nwjs-test-mode") ?
    nw::package(&extension_path) : nw::package();
  manifest->SetBoolean(manifest_keys::kNWJSInternalFlag, true);
  if (error && !package->cached_error_content().empty()) {
    *error = package->cached_error_content();
    return;
  }

  manifest->Set(manifest_keys::kNWJSInternalManifest, base::WrapUnique(package->root()->DeepCopy()));

  if (manifest->GetString(manifest_keys::kNWJSMain, &main_url)) {
    if (base::EndsWith(main_url, ".js", base::CompareCase::INSENSITIVE_ASCII)) {
      AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, main_url);
      manifest->SetString(manifest_keys::kNWJSInternalMainFilename, main_url);
    }else if (base::FeatureList::IsEnabled(::features::kNWNewWin))
      AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, "nwjs/newwin.js");
    else
      AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, "nwjs/default.js");

    std::string bg_script;
    if (manifest->GetString("bg-script", &bg_script))
      AmendManifestStringList(manifest, manifest_keys::kPlatformAppBackgroundScripts, bg_script);

    AmendManifestStringList(manifest, manifest_keys::kPermissions, "developerPrivate");
    AmendManifestStringList(manifest, manifest_keys::kPermissions, "management");
    AmendManifestStringList(manifest, manifest_keys::kPermissions, "<all_urls>");
  }

  if (manifest->GetString("window.icon", &icon_path)) {
    gfx::Image app_icon;
    if (GetPackageImage(package, base::FilePath::FromUTF8Unsafe(icon_path), &app_icon)) {
      if (app_icon.Width() > 128 || app_icon.Height() > 128) {
        const gfx::ImageSkia* originImageSkia = app_icon.ToImageSkia();
        gfx::ImageSkia resizedImageSkia =
          gfx::ImageSkiaOperations::CreateResizedImage(*originImageSkia,
                                                       skia::ImageOperations::RESIZE_GOOD,
                                                       gfx::Size(128, 128));
        app_icon = gfx::Image(resizedImageSkia);
      }
      SetAppIcon(app_icon);
      int width = app_icon.Width();
      std::string key = "icons." + base::NumberToString(width);
      manifest->SetString(key, icon_path);
#if defined(OS_WIN)
      SetWindowHIcon((IconUtil::CreateHICONFromSkBitmapSizedTo(*app_icon.AsImageSkia().bitmap(),
                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON))));
      SetAppHIcon((IconUtil::CreateHICONFromSkBitmapSizedTo(*app_icon.AsImageSkia().bitmap(),
                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON))));
#endif
    }
  }

  if (manifest->Get(switches::kmRemotePages, &node_remote)) {
    //FIXME: node-remote spec different with kWebURLs
    std::string node_remote_string;
    base::ListValue* node_remote_list = NULL;
    if (node_remote->GetAsString(&node_remote_string)) {
      node_remote_list = new base::ListValue();
      node_remote_list->Append(base::WrapUnique(new base::Value(node_remote_string)));
    } else if (node_remote->GetAsList(&node_remote_list)) {
      // do nothing
    }
    if (node_remote_list)
      AmendManifestList(manifest, manifest_keys::kWebURLs, *node_remote_list);
  }

  if (NWContentVerifierDelegate::GetDefaultMode() == NWContentVerifierDelegate::ENFORCE_STRICT)
    manifest->SetBoolean(manifest_keys::kNWJSContentVerifyFlag, true);

  if (package->temp_dir().IsValid()) {
    // need to remove the dir in renderer process
    manifest->SetString("nw-temp-dir", package->temp_dir().GetPath().AsUTF8Unsafe());
  }
}

void CalcNewWinParams(content::WebContents* new_contents, void* params,
                      std::string* nw_inject_js_doc_start,
                      std::string* nw_inject_js_doc_end,
                      const std::string& in_manifest) {
  extensions::AppWindow::CreateParams ret;
  std::unique_ptr<base::Value> val;
  std::unique_ptr<base::DictionaryValue> manifest = MergeManifest(in_manifest);

  bool resizable;
  if (manifest->GetBoolean(switches::kmResizable, &resizable)) {
    ret.resizable = resizable;
  }
  bool fullscreen;
  if (manifest->GetBoolean(switches::kmFullscreen, &fullscreen) && fullscreen) {
    ret.state = ui::SHOW_STATE_FULLSCREEN;
  }
  int width = 0, height = 0;
  if (manifest->GetInteger(switches::kmWidth, &width))
    ret.content_spec.bounds.set_width(width);
  if (manifest->GetInteger(switches::kmHeight, &height))
    ret.content_spec.bounds.set_height(height);

  int x = 0, y = 0;
  if (manifest->GetInteger(switches::kmX, &x))
    ret.window_spec.bounds.set_x(x);
  if (manifest->GetInteger(switches::kmY, &y))
    ret.window_spec.bounds.set_y(y);
  bool top;
  if (manifest->GetBoolean(switches::kmAlwaysOnTop, &top) && top) {
    ret.always_on_top = true;
  }
  bool frame;
  if (manifest->GetBoolean(switches::kmFrame, &frame) && !frame) {
    ret.frame = extensions::AppWindow::FRAME_NONE;
  }
  bool all_workspaces;
  if (manifest->GetBoolean(switches::kmVisibleOnAllWorkspaces, &all_workspaces)
    && all_workspaces) {
    ret.visible_on_all_workspaces = true;
  }
  gfx::Size& minimum_size = ret.content_spec.minimum_size;
  int min_height = 0, min_width = 0;
  if (manifest->GetInteger(switches::kmMinWidth, &min_width))
    minimum_size.set_width(min_width);
  if (manifest->GetInteger(switches::kmMinHeight, &min_height))
    minimum_size.set_height(min_height);
  int max_height = 0, max_width = 0;
  gfx::Size& maximum_size = ret.content_spec.maximum_size;
  if (manifest->GetInteger(switches::kmMaxWidth, &max_width))
    maximum_size.set_width(max_width);
  if (manifest->GetInteger(switches::kmMaxHeight, &max_height))
    maximum_size.set_height(max_height);

  *(extensions::AppWindow::CreateParams*)params = ret;

  manifest->GetString(switches::kmInjectJSDocStart, nw_inject_js_doc_start);
  manifest->GetString(switches::kmInjectJSDocEnd, nw_inject_js_doc_end);
}

bool ExecuteAppCommandHook(int command_id, extensions::AppWindow* app_window) {
#if defined(OS_MAC)
  return false;
#else
  //nw::ObjectManager* obj_manager = nw::ObjectManager::Get(app_window->browser_context());
  //Menu* menu = (Menu*)obj_manager->GetApiObject(command_id);
  Menu* menu = app_window->menu_;
  if (!menu)
    return false;
  menu->menu_delegate_->ExecuteCommand(command_id, 0);
  return true;
#endif //OSX
}

} // namespace nw
