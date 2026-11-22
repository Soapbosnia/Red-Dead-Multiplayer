        <UIScrollableList id="SinglePlayerList" MaxHeight="5" allowWrap="true" allowMouseHoverSelect="true" allowMouseAutoSelect="true">
          <!-- Red Dead Multiplayer main menu (replaces the game's mode list; src/game/uifiles.cpp). Texts: Rdmp* ids. -->
          <UILabel id="RdmpMenu_Browser" text="Common_Null" icon="{@UI.ACCEPT}" value="5592405">
            <transition event="@UI.ACCEPT*RELEASED">
              <action expr="PlaySound('HUD_MENU_SELECT_MASTER')"></action>
              <action expr="StackPush(PauseMenuScene)"></action>
              <action expr="MomentarilyDisableInput(0.25)"></action>
              <action expr="SendDelayedEvent('rdmp.openBrowser', 0.25)"></action>
            </transition>
          </UILabel>
          <UILabel id="RdmpMenu_Direct" text="Common_Null" icon="{@UI.ACCEPT}" value="5592405">
            <transition event="@UI.ACCEPT*RELEASED">
              <action expr="PlaySound('HUD_MENU_SELECT_MASTER')"></action>
              <action expr="SendEvent('rdmp.direct')"></action>
            </transition>
          </UILabel>
          <UILabel id="RdmpMenu_MpSettings" text="Common_Null" icon="{@UI.ACCEPT}" value="5592405">
            <transition event="@UI.ACCEPT*RELEASED">
              <action expr="PlaySound('HUD_MENU_SELECT_MASTER')"></action>
              <action expr="SendEvent('rdmp.mpsettings')"></action>
            </transition>
          </UILabel>
          <UILabel id="RdmpMenu_GameSettings" text="Common_Settings" icon="{@UI.ACCEPT}" value="5592405">
            <transition event="@UI.ACCEPT*RELEASED">
              <action expr="PlaySound('HUD_MENU_SELECT_MASTER')"></action>
              <action expr="StackPush(PauseMenuScene)"></action>
              <action expr="MomentarilyDisableInput(0.25)"></action>
              <action expr="SendDelayedEvent('@UI.ACCEPT*RELEASED', 0.25)"></action>
            </transition>
          </UILabel>
          <UILabel id="RdmpMenu_Quit" text="Common_Quit" icon="{@UI.ACCEPT}" value="5592405">
            <transition event="@UI.ACCEPT*RELEASED">
              <action expr="Enter(QuitGame)"></action>
            </transition>
          </UILabel>
        </UIScrollableList>
