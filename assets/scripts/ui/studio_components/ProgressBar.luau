
local React = require("React")

local Constants = { DefaultProgressBarHeight = 18, DefaultFont = Enum.Font.SourceSans, DefaultTextSize = 13 }
local useTheme = require("scripts/ui/studio_components/Theme.luau").useTheme


type ProgressBarProps = {
	Value: number,
	Max: number?,
	Formatter: ((value: number, max: number) -> string)?,
}

local function defaultFormatter(value: number, max: number)
	return string.format("%i%%", 100 * value / max)
end

local function ProgressBar(props: ProgressBarProps)
	local theme = useTheme()

	local formatter: (number, number) -> string = defaultFormatter
	if props.Formatter then
		formatter = props.Formatter
	end

	local max = math.max(props.Max or 1, 0.0001)
	local value = math.clamp(props.Value, 0, max)
	local alpha = value / max
	local textScale = if alpha > 0 then 1 / alpha else 0
	local text = formatter(value, max)

	local modifier = "Default"
	if props.Disabled then
		modifier = "Disabled"
	end

	return React.createElement("Frame", {
		AnchorPoint = props.AnchorPoint,
		Position = props.Position,
		Size = props.Size or UDim2.new(1, 0, 0, Constants.DefaultProgressBarHeight),
		LayoutOrder = props.LayoutOrder,
		ZIndex = props.ZIndex,
		BackgroundColor3 = theme:GetColor("InputFieldBackground", modifier),
		BorderColor3 = theme:GetColor("InputFieldBorder", modifier),
	}, {
		Bar = React.createElement("Frame", {
			BackgroundColor3 = theme:GetColor("DialogMainButton", modifier),
			BorderSizePixel = 0,
			Size = UDim2.fromScale(alpha, 1),
			ClipsDescendants = true,
			ZIndex = 1,
		}, {
			Left = React.createElement("TextLabel", {
				BackgroundTransparency = 1,
				Size = UDim2.fromScale(textScale, 1) - UDim2.fromOffset(0, 1),
				Font = Constants.DefaultFont,
				TextSize = Constants.DefaultTextSize,
				TextColor3 = Color3.fromRGB(12, 12, 12),
				TextTransparency = if props.Disabled then 0.5 else 0,
				Text = text,
			}),
		}),
		Under = React.createElement("TextLabel", {
			BackgroundTransparency = 1,
			Size = UDim2.fromScale(1, 1) - UDim2.fromOffset(0, 1),
			Font = Constants.DefaultFont,
			TextSize = Constants.DefaultTextSize,
			TextColor3 = theme:GetColor("MainText", modifier),
			Text = text,
			ZIndex = 0,
		}),
	})
end

return ProgressBar
