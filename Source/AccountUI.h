#pragma once

#include "Account.h"
#include "Widgets.h"

// Round profile picture; falls back to the email's first letter on a violet gem gradient.
void drawAvatar (juce::Graphics&, juce::Rectangle<float> area, const juce::Image& picture, const juce::String& email, bool pro);

// Toolbar account button: avatar + plan text.
class AccountBadge : public juce::Button
{
public:
    AccountBadge() : juce::Button ("Account") {}

    void setInfo (const juce::Image& picture, const juce::String& emailText, const juce::String& label, bool isPro, bool signedIn)
    {
        avatar = picture; email = emailText; text = label; pro = isPro; showAvatar = signedIn;
        repaint();
    }

    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    juce::Image avatar;
    juce::String email, text { "Sign in" };
    bool pro = false, showAvatar = false;
};

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
    juce::TextButton changePicBtn { "Change picture..." }, removePicBtn { "Remove" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> avatarArea;
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
