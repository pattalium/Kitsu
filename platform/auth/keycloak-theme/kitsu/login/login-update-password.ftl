<#import "template.ftl" as layout>
<#import "password-commons.ftl" as passwordCommons>
<#import "field.ftl" as field>
<#import "buttons.ftl" as buttons>
<#import "password-validation.ftl" as validator>
<@layout.registrationLayout displayMessage=!messagesPerField.existsError('password','password-confirm'); section>
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
                    <span class="kitsu-brand__purpose">Secure first sign-in</span>
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
        <div class="kitsu-auth-intro" data-testid="kitsu-password-intro">
            <h1>${msg("kitsuChoosePasswordTitle")}</h1>
            <p>${msg("kitsuChoosePasswordHelp")}</p>
        </div>
        <form id="kc-passwd-update-form"
              class="${properties.kcFormClass!}"
              onsubmit="login.disabled = true; return true;"
              action="${url.loginAction}"
              method="post"
              novalidate="novalidate">
            <@field.password name="password-new"
                             label=msg("passwordNew")
                             fieldName="password"
                             autocomplete="new-password"
                             autofocus=true />
            <@field.password name="password-confirm"
                             label=msg("passwordConfirm")
                             autocomplete="new-password" />
            <div class="${properties.kcFormGroupClass!}">
                <@passwordCommons.logoutOtherSessions />
            </div>
            <@buttons.actionGroup horizontal=true>
                <#if isAppInitiatedAction??>
                    <@buttons.button id="kc-submit"
                                     name="login"
                                     label="doSubmit"
                                     class=["kcButtonPrimaryClass"] />
                    <@buttons.button id="kc-cancel"
                                     label="doCancel"
                                     name="cancel-aia"
                                     class=["kcButtonSecondaryClass"] />
                <#else>
                    <@buttons.button id="kc-submit"
                                     name="login"
                                     label="doSubmit"
                                     class=["kcButtonPrimaryClass", "kcButtonBlockClass"] />
                </#if>
            </@buttons.actionGroup>
        </form>
        <@validator.templates />
        <@validator.script field="password-new" />
    </#if>
</@layout.registrationLayout>
