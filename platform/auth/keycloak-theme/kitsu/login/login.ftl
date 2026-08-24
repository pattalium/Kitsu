<#import "template.ftl" as layout>
<#import "field.ftl" as field>
<#import "buttons.ftl" as buttons>
<#import "social-providers.ftl" as identityProviders>
<#import "passkeys.ftl" as passkeys>
<@layout.registrationLayout displayMessage=!messagesPerField.existsError('username','password') displayInfo=false; section>
    <#if section = "header">
        <div class="kitsu-brand-row">
            <div class="kitsu-brand" data-testid="kitsu-auth-brand">
                <img class="kitsu-brand__mascot"
                     src="${url.resourcesPath}/img/kitsu-k32-mascot-bw-v2.png"
                     alt="Kitsu"
                     width="88"
                     height="88">
                <div>
                    <span class="kitsu-brand__name">Kitsu K32</span>
                    <span class="kitsu-brand__purpose">Private companion access</span>
                </div>
            </div>
            <button class="kitsu-theme-toggle"
                    type="button"
                    data-kitsu-theme-toggle
                    aria-label="Switch to dark theme"
                    aria-pressed="false">
                <span>Theme</span>
                <span class="kitsu-theme-toggle__value" data-kitsu-theme-label>Light</span>
            </button>
        </div>
    <#elseif section = "form">
        <div class="kitsu-auth-intro" data-testid="kitsu-auth-intro">
            <h1>${msg("kitsuOwnerAccessTitle")}</h1>
            <p>${msg("kitsuOwnerAccessIntro")}</p>
            <p class="kitsu-local-note">${msg("kitsuBluetoothNoAccount")}</p>
        </div>
        <div id="kc-form">
            <div id="kc-form-wrapper">
                <#if realm.password>
                    <form id="kc-form-login"
                          class="${properties.kcFormClass!}"
                          onsubmit="login.disabled = true; return true;"
                          action="${url.loginAction}"
                          method="post"
                          novalidate="novalidate">
                        <#if !usernameHidden??>
                            <@field.input name="username"
                                          label=msg("kitsuOwnerUsername")
                                          error=kcSanitize(messagesPerField.getFirstError('username','password'))?no_esc
                                          autofocus=true
                                          autocomplete="${(enableWebAuthnConditionalUI?has_content)?then('username webauthn', 'username')}"
                                          value=login.username!'' />
                            <@field.password name="password"
                                             label=msg("password")
                                             error=""
                                             forgotPassword=realm.resetPasswordAllowed
                                             autofocus=usernameHidden??
                                             autocomplete="current-password">
                                <#if realm.rememberMe && !usernameHidden??>
                                    <@field.checkbox name="rememberMe"
                                                     label=msg("rememberMe")
                                                     value=login.rememberMe?? />
                                </#if>
                            </@field.password>
                        <#else>
                            <@field.password name="password"
                                             label=msg("password")
                                             forgotPassword=realm.resetPasswordAllowed
                                             autofocus=usernameHidden??
                                             autocomplete="current-password">
                                <#if realm.rememberMe && !usernameHidden??>
                                    <@field.checkbox name="rememberMe"
                                                     label=msg("rememberMe")
                                                     value=login.rememberMe?? />
                                </#if>
                            </@field.password>
                        </#if>
                        <input type="hidden"
                               id="id-hidden-input"
                               name="credentialId"
                               <#if auth.selectedCredential?has_content>value="${auth.selectedCredential}"</#if>>
                        <@buttons.loginButton />
                    </form>
                </#if>
            </div>
        </div>
        <@passkeys.conditionalUIData />
        <aside class="kitsu-first-login" aria-labelledby="kitsu-first-login-title">
            <h2 id="kitsu-first-login-title">${msg("kitsuFirstLoginTitle")}</h2>
            <p>${msg("kitsuFirstLoginHelp")}</p>
        </aside>
        <aside class="kitsu-account-help" aria-labelledby="kitsu-account-help-title">
            <h2 id="kitsu-account-help-title">${msg("kitsuRecoveryTitle")}</h2>
            <p>${msg("kitsuRecoveryHelp")}</p>
            <a href="https://docs.k32.run/android/#owner-account"
               target="_blank"
               rel="noopener noreferrer">${msg("kitsuRecoveryLink")}</a>
        </aside>
    <#elseif section = "socialProviders">
        <#if realm.password && social.providers?? && social.providers?has_content>
            <@identityProviders.show social=social />
        </#if>
    </#if>
</@layout.registrationLayout>
