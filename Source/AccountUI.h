#pragma once

#include "Account.h"
#include "Widgets.h"

// Sign in / create account / plan status.
class AccountPanel : public juce::Component, private juce::ChangeListener
{
public:
    explicit AccountPanel (Account&);
    ~AccountPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void show (Account&, juce::Component* centreAround);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();

    Account& account;
    juce::TextEditor emailBox, passwordBox;
    juce::TextButton signInBtn { "Sign in" }, signUpBtn { "Create account" }, forgotBtn { "Forgot password?" };
    juce::TextButton refreshBtn { "Refresh status" }, upgradeBtn { "Get Pro" }, signOutBtn { "Sign out" };
    juce::Image bg;
};

// Shown when a Free user reaches for a Pro feature.
class ProPrompt : public juce::Component
{
public:
    ProPrompt (Account&, juce::String featureName, std::function<void()> openAccount);

    void paint (juce::Graphics&) override;
    void resized() override;

    static void show (Account&, const juce::String& featureName, juce::Component* centreAround, std::function<void()> openAccount);

private:
    void close();

    Account& account;
    juce::String feature;
    std::function<void()> openAccount;
    juce::TextButton primaryBtn, laterBtn { "Not now" };
    juce::Image bg;
};
