-- TurboGameEngine adapter for the imported MIT StudioComponents examples.
-- Studio's theme service is replaced by ordinary React context. This module
-- intentionally provides only the colors consumed by LoadingDots/ProgressBar.
local React = require("React")
local fallback = {
	accent = Color3.fromRGB(90, 220, 210),
	track = Color3.fromRGB(27, 43, 57),
	text = Color3.fromRGB(227, 237, 241),
	border = Color3.fromRGB(48, 66, 82),
}
local Context = React.createContext(fallback)
local Theme = { Context = Context }
function Theme.useTheme()
	local colors = React.useContext(Context)
	return {
		GetColor = function(_, key)
			if key == "InputFieldBackground" then return colors.track end
			if key == "InputFieldBorder" or key == "ButtonBorder" then return colors.border end
			if key == "DialogMainButton" or key == "ButtonText" then return colors.accent end
			return colors.text
		end,
	}
end
return Theme
