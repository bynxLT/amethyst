#pragma once

#include "Account.h"
#include "Widgets.h"

// Round profile picture; falls back to the email's first letter on a violet gem gradient.
void drawAvatar (juce::Graphics&, juce::Rectangle<float> area, const juce::Image& picture, const juce::String& email, bool pro);

// Toolbar account button: avatar + plan text.
class AccountBadge : public juce::Button, private juce::Timer
{
public:
    AccountBadge() : juce::Button ("Account") {}

    // tier: 0 free, 1 Pro (gem), 2 Ultra Pro (crown + slow shimmer)
    void setInfo (const juce::Image& picture, const juce::String& emailText, const juce::String& label, int planTier, bool signedIn)
    {
        avatar = picture; email = emailText; text = label; tier = planTier; pro = planTier >= 1; showAvatar = signedIn;
        if (tier >= 2 || (pro && avatar.getWidth() >= avatar.getHeight() * 2)) startTimerHz (25); else stopTimer();   // shimmer / animated picture
        repaint();
    }

    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    void timerCallback() override { shimmer += 0.012f; if (shimmer > 1.6f) shimmer = -0.6f; repaint(); }

    juce::Image avatar;
    juce::String email, text { "Sign in" };
    bool pro = false, showAvatar = false;
    int tier = 0;
    float shimmer = -0.6f;
};

// Sign in / create account / plan status.
class AccountPanel : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit AccountPanel (Account&);
    ~AccountPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void show (Account&, juce::Component* centreAround);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override;
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
    ProPrompt (Account&, juce::String featureName, int requiredTier, std::function<void()> openAccount);

    void paint (juce::Graphics&) override;
    void resized() override;

    static void show (Account&, const juce::String& featureName, int requiredTier, juce::Component* centreAround, std::function<void()> openAccount);

private:
    void close();

    Account& account;
    juce::String feature;
    int tier = 1;
    std::function<void()> openAccount;
    juce::TextButton primaryBtn, laterBtn { "Not now" };
    juce::Image bg;
};
